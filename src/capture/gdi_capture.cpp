// capture/gdi_capture.cpp — GDI BitBlt fallback capture backend (design doc §5.5).
//
// This path exists for the desktops Desktop Duplication refuses: protected
// content, a locked session, or a driver that will not hand out a duplication
// at all. It has no hardware handoff, so it bit-blits into a DIB section and
// uploads to the GPU itself, but it shares the exact same publishing scheme as
// the DXGI path, so resolve_texture behaves identically for the renderer.
#include "capture/capture.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mag {
namespace {

constexpr std::size_t kRingSize = 3;
constexpr std::size_t kTokenHistory = 8;
// A DIB-to-D3D upload cannot be signalled, so this path polls. ~80 Hz is well
// inside the fallback's latency budget and keeps a static desktop cheap.
constexpr std::chrono::milliseconds kPollInterval{12};
constexpr std::chrono::milliseconds kForcedPublishInterval{500};
constexpr std::size_t kSampleCount = 8192;
constexpr std::uint32_t kMaxAcquireTimeoutMs = 1000;
constexpr std::uint32_t kIdleBackoffMs = 50;

// ErrorReported codes. capture.h freezes no enum for these, so the capture
// module owns this small numbering (kept identical across the three TUs).
constexpr std::uint32_t kCodeSessionLost = 2;

// These literals are the only ErrorReported.text values on this path because
// the frozen event carries a bare const char* that must outlive the publish.
constexpr const char* kTextSessionLost = "GDI capture failed; capture suspended";
constexpr const char* kTextRecoveryFailed = "Capture recovery failed; GDI capture unavailable";

std::atomic<std::uint64_t> g_token_counter{1};

std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}

std::string hresult_text(HRESULT hr) {
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

template <typename T>
void publish(BoundedEventBus& bus, T payload) noexcept {
    try {
        AppEvent event = AppEvent::make(std::move(payload));
        event.timestamp_qpc = qpc_now();
        event.source_thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
        bus.publish(event);
    } catch (...) {
        // A capture thread must survive a full bus.
    }
}

struct RingSlot {
    std::uint32_t index{0};
    ID3D11Texture2D* texture{nullptr};
    IDXGIKeyedMutex* keyed_mutex{nullptr};
    HANDLE shared_handle{nullptr};
    SizePx size_px{};
    TextureToken token{kInvalidToken};
};

struct TokenRecord {
    TextureToken token{kInvalidToken};
    std::uint32_t slot{0};
    std::uint64_t generation{0};
};

struct Resources {
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* context{nullptr};
    ID3D11Texture2D* staging{nullptr};
    HDC screen_dc{nullptr};
    HDC mem_dc{nullptr};
    HBITMAP dib{nullptr};
    HGDIOBJ old_bitmap{nullptr};
    void* dib_bits{nullptr};
    RectPx desktop_rect_px{};
    SizePx size_px{};
    std::array<RingSlot, kRingSize> ring{};

    Resources() = default;
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;
    Resources(Resources&& other) noexcept { adopt(other); }
    Resources& operator=(Resources&& other) noexcept {
        if (this != &other) {
            release();
            adopt(other);
        }
        return *this;
    }
    ~Resources() { release(); }

    void release() noexcept {
        release_ring();
        if (staging) {
            staging->Release();
            staging = nullptr;
        }
        if (context) {
            context->Release();
            context = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (mem_dc && old_bitmap) {
            SelectObject(mem_dc, old_bitmap);
            old_bitmap = nullptr;
        }
        if (dib) {
            DeleteObject(dib);
            dib = nullptr;
            dib_bits = nullptr;
        }
        if (mem_dc) {
            DeleteDC(mem_dc);
            mem_dc = nullptr;
        }
        if (screen_dc) {
            ReleaseDC(nullptr, screen_dc);
            screen_dc = nullptr;
        }
    }

    void release_ring() noexcept {
        for (RingSlot& slot : ring) {
            if (slot.keyed_mutex) {
                slot.keyed_mutex->Release();
                slot.keyed_mutex = nullptr;
            }
            if (slot.texture) {
                slot.texture->Release();
                slot.texture = nullptr;
            }
            if (slot.shared_handle) {
                CloseHandle(slot.shared_handle);
                slot.shared_handle = nullptr;
            }
            slot.token = kInvalidToken;
        }
    }

private:
    void adopt(Resources& other) noexcept {
        device = other.device;
        context = other.context;
        staging = other.staging;
        screen_dc = other.screen_dc;
        mem_dc = other.mem_dc;
        dib = other.dib;
        old_bitmap = other.old_bitmap;
        dib_bits = other.dib_bits;
        desktop_rect_px = other.desktop_rect_px;
        size_px = other.size_px;
        ring = other.ring;

        other.device = nullptr;
        other.context = nullptr;
        other.staging = nullptr;
        other.screen_dc = nullptr;
        other.mem_dc = nullptr;
        other.dib = nullptr;
        other.old_bitmap = nullptr;
        other.dib_bits = nullptr;
        for (RingSlot& slot : other.ring) {
            slot.texture = nullptr;
            slot.keyed_mutex = nullptr;
            slot.shared_handle = nullptr;
        }
    }
};

// Throws CaptureUnavailable; the caller's Resources local unwinds what was
// already built.
Resources build_resources(RectPx requested_rect_px) {
    Resources res;

    RectPx rect = requested_rect_px;
    if (is_empty(rect)) {
        // GDI has no per-output object to ask, so the virtual desktop is the
        // only honest default when the caller has no topology yet.
        const Px vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const Px vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        rect = RectPx{vx, vy, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                      vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    }
    if (is_empty(rect)) {
        throw CaptureUnavailable("GDI capture needs a non-empty desktop rect");
    }
    res.desktop_rect_px = rect;
    res.size_px = size_of(rect);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                   D3D11_SDK_VERSION, &res.device, nullptr, &res.context);
    if (FAILED(hr) || res.device == nullptr) {
        // A machine without a usable GPU still deserves a magnifier, so WARP is
        // the last resort rather than failing the fallback as well.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3,
                               D3D11_SDK_VERSION, &res.device, nullptr, &res.context);
    }
    if (FAILED(hr) || res.device == nullptr) {
        throw CaptureUnavailable("GDI fallback D3D11CreateDevice failed: " + hresult_text(hr));
    }

    res.screen_dc = GetDC(nullptr);
    if (res.screen_dc == nullptr) {
        throw CaptureUnavailable("GetDC(NULL) failed");
    }
    res.mem_dc = CreateCompatibleDC(res.screen_dc);
    if (res.mem_dc == nullptr) {
        throw CaptureUnavailable("CreateCompatibleDC failed");
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = res.size_px.width;
    // Negative height selects a top-down DIB, so its rows already run in the
    // same direction as a D3D11 texture and the upload is a straight memcpy.
    info.bmiHeader.biHeight = -res.size_px.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    res.dib = CreateDIBSection(res.screen_dc, &info, DIB_RGB_COLORS, &res.dib_bits, nullptr, 0);
    if (res.dib == nullptr || res.dib_bits == nullptr) {
        throw CaptureUnavailable("CreateDIBSection failed");
    }
    res.old_bitmap = SelectObject(res.mem_dc, res.dib);
    if (res.old_bitmap == nullptr || res.old_bitmap == HGDI_ERROR) {
        res.old_bitmap = nullptr;
        throw CaptureUnavailable("SelectObject(DIB) failed");
    }

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = static_cast<UINT>(res.size_px.width);
    staging_desc.Height = static_cast<UINT>(res.size_px.height);
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_DYNAMIC;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    // A zero BindFlags texture is rejected with E_INVALIDARG, so the staging
    // copy source needs one real binding even though we never sample it.
    staging_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(res.device->CreateTexture2D(&staging_desc, nullptr, &res.staging)) ||
        res.staging == nullptr) {
        throw CaptureUnavailable("GDI fallback staging texture creation failed");
    }

    for (std::size_t i = 0; i < kRingSize; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(res.size_px.width);
        desc.Height = static_cast<UINT>(res.size_px.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        ID3D11Texture2D* texture = nullptr;
        if (FAILED(res.device->CreateTexture2D(&desc, nullptr, &texture)) || texture == nullptr) {
            throw CaptureUnavailable("GDI fallback shared texture creation failed");
        }
        IDXGIResource1* resource1 = nullptr;
        if (FAILED(texture->QueryInterface(__uuidof(IDXGIResource1),
                                           reinterpret_cast<void**>(&resource1))) ||
            resource1 == nullptr) {
            texture->Release();
            throw CaptureUnavailable("IDXGIResource1 unavailable on shared texture");
        }
        HANDLE handle = nullptr;
        const HRESULT shr = resource1->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        resource1->Release();
        if (FAILED(shr) || handle == nullptr) {
            texture->Release();
            throw CaptureUnavailable("CreateSharedHandle failed: " + hresult_text(shr));
        }
        IDXGIKeyedMutex* keyed = nullptr;
        if (FAILED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex),
                                           reinterpret_cast<void**>(&keyed))) ||
            keyed == nullptr) {
            CloseHandle(handle);
            texture->Release();
            throw CaptureUnavailable("IDXGIKeyedMutex unavailable on shared texture");
        }
        res.ring[i].index = static_cast<std::uint32_t>(i);
        res.ring[i].texture = texture;
        res.ring[i].keyed_mutex = keyed;
        res.ring[i].shared_handle = handle;
        res.ring[i].size_px = res.size_px;
        res.ring[i].token = kInvalidToken;
    }
    return res;
}

class GdiCaptureBackend final : public ICaptureBackend {
public:
    explicit GdiCaptureBackend(BoundedEventBus& bus) noexcept : bus_(bus) {}
    ~GdiCaptureBackend() override { stop(); }

    GdiCaptureBackend(const GdiCaptureBackend&) = delete;
    GdiCaptureBackend& operator=(const GdiCaptureBackend&) = delete;

    void start(std::uint32_t output_id, RectPx desktop_rect_px) override {
        stop();
        output_id_ = output_id;
        requested_rect_px_ = desktop_rect_px;
        // stop() left the loop flags armed for the previous thread; a fresh
        // start has to clear them or the new worker exits immediately.
        stop_requested_.store(false, std::memory_order_release);
        recover_requested_.store(false, std::memory_order_release);
        faulted_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_.clear();
        }
        Resources built = build_resources(desktop_rect_px);
        install(built);
        last_seen_index_ = 0;
        worker_ = std::thread([this] { run(); });
    }

    std::optional<GpuFrame> acquire(std::uint32_t timeout_ms) override {
        const std::uint32_t bounded = std::min(timeout_ms, kMaxAcquireTimeoutMs);
        std::unique_lock<std::mutex> lock(mutex_);
        if (device_ == nullptr && !worker_.joinable()) {
            throw std::logic_error("GDI acquire() called before start()");
        }
        cv_.wait_for(lock, std::chrono::milliseconds(bounded), [this] {
            return frame_index_ > last_seen_index_ || faulted_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire);
        });
        if (frame_index_ > last_seen_index_) {
            last_seen_index_ = frame_index_;
            return latest_;
        }
        if (faulted_.load(std::memory_order_acquire)) {
            throw DeviceRemoved(last_error_);
        }
        return std::nullopt;
    }

    void stop() noexcept override {
        stop_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
        try {
            if (worker_.joinable()) {
                worker_.join();
            }
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            release_cache_locked();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_locked();
        }
        try {
            worker_ = std::thread{};
        } catch (...) {
        }
    }

    void recover_async() noexcept override {
        recover_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    ID3D11Texture2D* resolve_texture(TextureToken token,
                                     ID3D11Device* render_device) noexcept override {
        try {
            if (token == kInvalidToken || render_device == nullptr) {
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            TokenRecord record{};
            const RingSlot* slot = handle_for_locked(token, record);
            if (slot == nullptr || slot->shared_handle == nullptr) {
                return nullptr;
            }

            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            if (cache_generation_ != record.generation || cache_device_ != render_device) {
                release_cache_locked();
                cache_generation_ = record.generation;
                cache_device_ = render_device;
            }
            auto it = cache_.find(record.slot);
            if (it != cache_.end()) {
                return it->second;
            }

            ID3D11Device1* device1 = nullptr;
            if (FAILED(render_device->QueryInterface(__uuidof(ID3D11Device1),
                                                     reinterpret_cast<void**>(&device1))) ||
                device1 == nullptr) {
                return nullptr;
            }
            ID3D11Texture2D* texture = nullptr;
            const HRESULT hr = device1->OpenSharedResource1(slot->shared_handle,
                                                            __uuidof(ID3D11Texture2D),
                                                            reinterpret_cast<void**>(&texture));
            device1->Release();
            if (FAILED(hr) || texture == nullptr) {
                return nullptr;
            }
            cache_.emplace(record.slot, texture);
            return texture;
        } catch (...) {
            return nullptr;
        }
    }

    void retire_texture(TextureToken token) noexcept override {
        try {
            if (token == kInvalidToken) {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            TokenRecord record{};
            if (handle_for_locked(token, record) == nullptr) {
                return;
            }
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            auto it = cache_.find(record.slot);
            if (it != cache_.end()) {
                if (it->second) {
                    it->second->Release();
                }
                cache_.erase(it);
            }
        } catch (...) {
        }
    }

    void release_all_textures() noexcept override {
        try {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            release_cache_locked();
        } catch (...) {
        }
    }

    const char* backend_name() const noexcept override { return "GDI BitBlt"; }
    bool is_fallback() const noexcept override { return true; }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    // Locking order is always mutex_ then cache_mutex_, never the reverse.
    const RingSlot* handle_for_locked(TextureToken token, TokenRecord& out) const {
        for (auto it = tokens_.rbegin(); it != tokens_.rend(); ++it) {
            if (it->token == token) {
                out = *it;
                return &ring_[it->slot];
            }
        }
        return nullptr;
    }

    void install(Resources& built) {
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
        device_ = built.device;
        context_ = built.context;
        staging_ = built.staging;
        screen_dc_ = built.screen_dc;
        mem_dc_ = built.mem_dc;
        dib_ = built.dib;
        old_bitmap_ = built.old_bitmap;
        dib_bits_ = built.dib_bits;
        desktop_rect_px_ = built.desktop_rect_px;
        size_px_ = built.size_px;
        ring_ = built.ring;

        built.device = nullptr;
        built.context = nullptr;
        built.staging = nullptr;
        built.screen_dc = nullptr;
        built.mem_dc = nullptr;
        built.dib = nullptr;
        built.old_bitmap = nullptr;
        built.dib_bits = nullptr;
        for (RingSlot& slot : built.ring) {
            slot.texture = nullptr;
            slot.keyed_mutex = nullptr;
            slot.shared_handle = nullptr;
        }
        ++generation_;
        ring_cursor_ = 0;
        tokens_.clear();
        previous_samples_.clear();
        previous_stride_ = 0;
    }

    void release_locked() noexcept {
        for (RingSlot& slot : ring_) {
            if (slot.keyed_mutex) {
                slot.keyed_mutex->Release();
                slot.keyed_mutex = nullptr;
            }
            if (slot.texture) {
                slot.texture->Release();
                slot.texture = nullptr;
            }
            if (slot.shared_handle) {
                CloseHandle(slot.shared_handle);
                slot.shared_handle = nullptr;
            }
            slot.token = kInvalidToken;
        }
        ring_cursor_ = 0;
        tokens_.clear();
        if (staging_) {
            staging_->Release();
            staging_ = nullptr;
        }
        if (context_) {
            context_->Release();
            context_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
        if (mem_dc_ && old_bitmap_) {
            SelectObject(mem_dc_, old_bitmap_);
            old_bitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
            dib_bits_ = nullptr;
        }
        if (mem_dc_) {
            DeleteDC(mem_dc_);
            mem_dc_ = nullptr;
        }
        if (screen_dc_) {
            ReleaseDC(nullptr, screen_dc_);
            screen_dc_ = nullptr;
        }
        previous_samples_.clear();
        previous_stride_ = 0;
    }

    void release_cache_locked() noexcept {
        for (auto& entry : cache_) {
            if (entry.second) {
                entry.second->Release();
            }
        }
        cache_.clear();
        cache_device_ = nullptr;
    }

    void set_fault(bool value, std::string message) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            faulted_.store(value, std::memory_order_release);
            last_error_ = std::move(message);
        }
        cv_.notify_all();
    }

    void run() noexcept {
        for (;;) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            if (recover_requested_.exchange(false, std::memory_order_acq_rel)) {
                attempt_recovery();
                continue;
            }
            if (faulted_.load(std::memory_order_acquire)) {
                // Retrying a BitBlt that the desktop is refusing would spin the
                // CPU for nothing; wait until the owner asks again.
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(kIdleBackoffMs), [this] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           recover_requested_.load(std::memory_order_acquire);
                });
                continue;
            }
            capture_once();
            std::this_thread::sleep_for(kPollInterval);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
    }

    void attempt_recovery() noexcept {
        try {
            Resources built = build_resources(requested_rect_px_);
            install(built);
            set_fault(false, std::string{});
            publish(bus_, CaptureRecovered{output_id_});
        } catch (const std::exception& error) {
            set_fault(true, error.what());
            publish(bus_, CaptureLost{output_id_});
            publish(bus_, ErrorReported{kCodeSessionLost, kTextRecoveryFailed});
        } catch (...) {
            set_fault(true, "GDI capture recovery failed");
            publish(bus_, CaptureLost{output_id_});
            publish(bus_, ErrorReported{kCodeSessionLost, kTextRecoveryFailed});
        }
    }

    void capture_once() noexcept {
        if (mem_dc_ == nullptr || screen_dc_ == nullptr || context_ == nullptr) {
            return;
        }
        // The screen DC uses virtual-desktop pixels with the primary monitor's
        // top-left as (0,0), so the rect's coordinates are already the right
        // BitBlt source offsets, negative ones included.
        if (!BitBlt(mem_dc_, 0, 0, size_px_.width, size_px_.height, screen_dc_,
                    desktop_rect_px_.left, desktop_rect_px_.top, SRCCOPY | CAPTUREBLT)) {
            handle_fault("BitBlt failed");
            return;
        }
        GdiFlush();
        if (!frame_changed()) {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            handle_fault("staging texture map failed");
            return;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(size_px_.width) * 4u;
        const auto* source = static_cast<const std::uint8_t*>(dib_bits_);
        auto* destination = static_cast<std::uint8_t*>(mapped.pData);
        for (Px y = 0; y < size_px_.height; ++y) {
            std::memcpy(destination + static_cast<std::size_t>(y) * mapped.RowPitch,
                        source + static_cast<std::size_t>(y) * row_bytes, row_bytes);
        }
        context_->Unmap(staging_, 0);

        copy_and_publish();
    }

    // Sampling a few thousand strided pixels costs far less than comparing a
    // whole 8 MB frame, and the forced republish covers whatever the samples
    // happen to miss.
    bool frame_changed() noexcept {
        const std::size_t pixel_count =
            static_cast<std::size_t>(size_px_.width) * static_cast<std::size_t>(size_px_.height);
        if (pixel_count == 0 || dib_bits_ == nullptr) {
            return false;
        }
        const std::size_t stride = std::max<std::size_t>(1, pixel_count / kSampleCount);
        const auto* current = static_cast<const std::uint32_t*>(dib_bits_);
        bool changed = false;

        if (previous_samples_.size() != kSampleCount || previous_stride_ != stride) {
            previous_samples_.assign(kSampleCount, 0);
            for (std::size_t i = 0; i < kSampleCount; ++i) {
                previous_samples_[i] = current[std::min(pixel_count - 1, i * stride)];
            }
            previous_stride_ = stride;
            changed = true;
        } else {
            for (std::size_t i = 0; i < kSampleCount; ++i) {
                const std::uint32_t value = current[std::min(pixel_count - 1, i * stride)];
                if (value != previous_samples_[i]) {
                    previous_samples_[i] = value;
                    changed = true;
                }
            }
        }
        if (changed) {
            return true;
        }
        return std::chrono::steady_clock::now() - last_publish_time_ >= kForcedPublishInterval;
    }

    void copy_and_publish() noexcept {
        for (std::size_t attempt = 0; attempt < kRingSize; ++attempt) {
            RingSlot& slot = ring_[ring_cursor_];
            ring_cursor_ = (ring_cursor_ + 1) % kRingSize;
            if (slot.texture == nullptr || slot.keyed_mutex == nullptr) {
                continue;
            }
            // Non-blocking: a slot the renderer still reads is skipped so the
            // upload never stalls behind presentation.
            if (slot.keyed_mutex->AcquireSync(0, 0) != S_OK) {
                continue;
            }
            context_->CopyResource(slot.texture, staging_);
            slot.keyed_mutex->ReleaseSync(0);
            publish_slot(slot);
            return;
        }
    }

    void publish_slot(RingSlot& slot) noexcept {
        const TextureToken token = g_token_counter.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex_);
        slot.token = token;
        tokens_.push_back(TokenRecord{token, slot.index, generation_});
        while (tokens_.size() > kTokenHistory) {
            tokens_.pop_front();
        }

        GpuFrame frame{};
        frame.texture_token = token;
        frame.output_id = output_id_;
        frame.desktop_rect_px = desktop_rect_px_;
        frame.texture_size_px = slot.size_px;
        frame.timestamp_qpc = qpc_now();
        frame.frame_index = ++frame_index_;
        frame.valid = true;
        latest_ = frame;
        last_publish_time_ = std::chrono::steady_clock::now();
        cv_.notify_all();
    }

    void handle_fault(const char* what) noexcept {
        set_fault(true, std::string(what) + " (error " + std::to_string(GetLastError()) + ")");
        publish(bus_, CaptureLost{output_id_});
        publish(bus_, ErrorReported{kCodeSessionLost, kTextSessionLost});
    }

    BoundedEventBus& bus_;
    std::uint32_t output_id_{0};
    RectPx requested_rect_px_{};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    ID3D11Texture2D* staging_{nullptr};
    HDC screen_dc_{nullptr};
    HDC mem_dc_{nullptr};
    HBITMAP dib_{nullptr};
    HGDIOBJ old_bitmap_{nullptr};
    void* dib_bits_{nullptr};
    RectPx desktop_rect_px_{};
    SizePx size_px_{};
    std::array<RingSlot, kRingSize> ring_{};
    std::size_t ring_cursor_{0};
    std::uint64_t generation_{0};
    std::deque<TokenRecord> tokens_{};
    GpuFrame latest_{};
    std::uint64_t frame_index_{0};
    std::uint64_t last_seen_index_{0};
    std::atomic<bool> faulted_{false};
    std::string last_error_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> recover_requested_{false};
    std::thread worker_;

    std::vector<std::uint32_t> previous_samples_;
    std::size_t previous_stride_{0};
    std::chrono::steady_clock::time_point last_publish_time_{};

    std::mutex cache_mutex_;
    std::unordered_map<std::uint32_t, ID3D11Texture2D*> cache_;
    ID3D11Device* cache_device_{nullptr};
    std::uint64_t cache_generation_{0};
};

}  // namespace

std::unique_ptr<ICaptureBackend> make_gdi_backend(BoundedEventBus& bus) {
    return std::make_unique<GdiCaptureBackend>(bus);
}

}  // namespace mag
