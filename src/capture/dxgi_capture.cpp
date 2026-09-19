// capture/dxgi_capture.cpp — DXGI Desktop Duplication backend (design doc §5.5).
//
// The duplication runs on its own thread and never hands a texture pointer
// across a thread boundary. Instead it writes each desktop frame into a ring of
// keyed-mutex textures and publishes an opaque token; the render thread opens
// that token's NT shared handle on its own device. That split is what keeps the
// D3D11 device single-threaded per contract while still letting the capture
// thread run ahead of presentation.
#include "capture/capture.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mag {
namespace {

constexpr std::size_t kRingSize = 3;
// A consumer may still hold the token it resolved for the previous frame, so
// the last few tokens stay resolvable; anything older is stale by definition.
constexpr std::size_t kTokenHistory = 8;
constexpr std::uint32_t kFrameWaitMs = 100;
constexpr std::uint32_t kMaxAcquireTimeoutMs = 1000;
constexpr std::uint32_t kIdleBackoffMs = 50;

// ErrorReported codes. capture.h freezes no enum for these, so the capture
// module owns this small numbering (kept identical across the three TUs).
constexpr std::uint32_t kCodeSessionLost = 2;
constexpr std::uint32_t kCodeDeviceRemoved = 3;

// These literals are the only ErrorReported.text values on this path because
// the frozen event carries a bare const char* that must outlive the publish.
constexpr const char* kTextSessionLost = "Desktop duplication lost; capture suspended";
constexpr const char* kTextRecoveryFailed = "Capture recovery failed; duplication unavailable";
constexpr const char* kTextDeviceRemoved = "Capture device removed";

// Monotonic across every session in the process, so a stale token can never be
// mistaken for a live one.
std::atomic<std::uint64_t> g_token_counter{1};

class DuplicationSession;

// One duplication per output per process is all DXGI permits; this registry
// lets a second backend attach to the live session instead of stacking one.
std::mutex g_registry_mutex;
std::map<std::uint32_t, std::weak_ptr<DuplicationSession>> g_registry;

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
        // A capture thread must survive a full bus; dropping the notification
        // is preferable to terminating the process.
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

// Everything a live capture needs. Built off-thread, then swapped in under the
// session mutex so the render thread never observes a half-rebuilt ring.
struct Resources {
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* context{nullptr};
    IDXGIOutputDuplication* duplication{nullptr};
    std::array<RingSlot, kRingSize> ring{};
    RectPx output_rect_px{};
    int rotation{0};

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
        if (duplication) {
            duplication->Release();
            duplication = nullptr;
        }
        if (context) {
            context->Release();
            context = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
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
        duplication = other.duplication;
        ring = other.ring;
        output_rect_px = other.output_rect_px;
        other.device = nullptr;
        other.context = nullptr;
        other.duplication = nullptr;
        other.output_rect_px = RectPx{};
        for (RingSlot& slot : other.ring) {
            slot.texture = nullptr;
            slot.keyed_mutex = nullptr;
            slot.shared_handle = nullptr;
        }
    }
};

// The magnifier addresses monitors by the stable ordinal the topology service
// assigns (EnumDisplayMonitors order), but DXGI numbers outputs per adapter.
// The two numbering schemes do not have to agree — an adapter with nothing
// attached still consumes an ordinal, and a machine can expose several
// adapters — so matching on the desktop rectangle is the reliable identity and
// the ordinal is kept only as a fallback for callers with no rectangle.
bool find_output(IDXGIFactory1* factory, std::uint32_t output_id, RectPx want_rect_px,
                 IDXGIAdapter1** out_adapter, IDXGIOutput** out_output) {
    const bool have_rect = !is_empty(want_rect_px);

    for (int pass = 0; pass < 2; ++pass) {
        const bool by_rect = (pass == 0) && have_rect;
        const bool by_ordinal = (pass == 1) || !have_rect;
        if (!by_rect && !by_ordinal) break;

        std::uint32_t ordinal = 0;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
                bool match = false;
                if (by_rect) {
                    DXGI_OUTPUT_DESC desc{};
                    if (SUCCEEDED(output->GetDesc(&desc))) {
                        const RECT& d = desc.DesktopCoordinates;
                        match = d.left == want_rect_px.left && d.top == want_rect_px.top &&
                                d.right == want_rect_px.right && d.bottom == want_rect_px.bottom;
                    }
                } else {
                    match = ordinal == output_id;
                }
                if (match) {
                    *out_adapter = adapter;
                    *out_output = output;
                    return true;
                }
                ++ordinal;
                output->Release();
            }
            adapter->Release();
        }
    }
    return false;
}

// Throws CaptureUnavailable with the failing HRESULT embedded, per §5.5.
Resources build_resources(std::uint32_t output_id, RectPx fallback_rect_px) {
    Resources res;

    IDXGIFactory1* factory = nullptr;
    const HRESULT fhr =
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(fhr) || factory == nullptr) {
        throw CaptureUnavailable("CreateDXGIFactory1 failed: " + hresult_text(fhr));
    }

    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    const bool found = find_output(factory, output_id, fallback_rect_px, &adapter, &output);
    factory->Release();
    if (!found) {
        throw CaptureUnavailable("no DXGI output matches monitor " + std::to_string(output_id) +
                                 " (desktop rect " + std::to_string(fallback_rect_px.left) + "," +
                                 std::to_string(fallback_rect_px.top) + " " +
                                 std::to_string(width_of(fallback_rect_px)) + "x" +
                                 std::to_string(height_of(fallback_rect_px)) + ")");
    }

    // The device must sit on the adapter that owns the output, or
    // DuplicateOutput refuses it, so the adapter is passed explicitly and the
    // driver type becomes UNKNOWN as D3D11 requires in that form.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1};
    const HRESULT dhr =
        D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 3,
                          D3D11_SDK_VERSION, &res.device, nullptr, &res.context);
    adapter->Release();
    if (FAILED(dhr) || res.device == nullptr) {
        output->Release();
        throw CaptureUnavailable("D3D11CreateDevice failed: " + hresult_text(dhr));
    }

    DXGI_OUTPUT_DESC output_desc{};
    output->GetDesc(&output_desc);
    res.output_rect_px =
        RectPx{output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
               output_desc.DesktopCoordinates.right, output_desc.DesktopCoordinates.bottom};
    if (is_empty(res.output_rect_px)) {
        res.output_rect_px = fallback_rect_px;
    }

    IDXGIOutput1* output1 = nullptr;
    const HRESULT qhr =
        output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    output->Release();
    if (FAILED(qhr) || output1 == nullptr) {
        throw CaptureUnavailable("IDXGIOutput1 unavailable for output " +
                                 std::to_string(output_id) + ": " + hresult_text(qhr));
    }

    const HRESULT hr = output1->DuplicateOutput(res.device, &res.duplication);
    output1->Release();
    if (FAILED(hr) || res.duplication == nullptr) {
        throw CaptureUnavailable("DuplicateOutput refused for output " +
                                 std::to_string(output_id) + ": " + hresult_text(hr));
    }

    // The surface arrives in the display's native orientation; on a pivoted
    // monitor that is not the desktop's, so the rotation travels with the frame
    // and the renderer undoes it when it works out its texture coordinates.
    DXGI_OUTDUPL_DESC duplication_desc{};
    res.duplication->GetDesc(&duplication_desc);
    switch (duplication_desc.Rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: res.rotation = 90; break;
        case DXGI_MODE_ROTATION_ROTATE180: res.rotation = 180; break;
        case DXGI_MODE_ROTATION_ROTATE270: res.rotation = 270; break;
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        default: res.rotation = 0; break;
    }
    return res;
}

class DuplicationSession final {
public:
    DuplicationSession(std::uint32_t output_id, RectPx requested_rect_px, BoundedEventBus& bus)
        : output_id_(output_id), requested_rect_px_(requested_rect_px), bus_(bus) {}

    ~DuplicationSession() { shutdown(); }

    DuplicationSession(const DuplicationSession&) = delete;
    DuplicationSession& operator=(const DuplicationSession&) = delete;

    // Throws CaptureUnavailable; the caller owns a shared_ptr already, so its
    // destructor still runs and unwinds any partially built resources.
    void initialize() {
        Resources built = build_resources(output_id_, requested_rect_px_);
        install(built);
    }

    void start_worker() {
        if (worker_.joinable()) {
            return;
        }
        worker_ = std::thread([this] { run(); });
    }

    std::optional<GpuFrame> wait_frame(std::uint64_t& last_seen_index, std::uint32_t timeout_ms) {
        const std::uint32_t bounded = std::min(timeout_ms, kMaxAcquireTimeoutMs);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(bounded), [this, last_seen_index] {
            return frame_index_ > last_seen_index || faulted_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire);
        });
        if (frame_index_ > last_seen_index) {
            last_seen_index = frame_index_;
            return latest_;
        }
        if (faulted_.load(std::memory_order_acquire)) {
            throw DeviceRemoved(last_error_);
        }
        return std::nullopt;
    }

    void request_recovery() noexcept {
        recover_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    void shutdown() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
        try {
            if (worker_.joinable()) {
                worker_.join();
            }
        } catch (...) {
            // join() can only fail for a misused thread object; never let that
            // escape a noexcept teardown.
        }
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
    }

    const RingSlot* handle_for(TextureToken token, TokenRecord& out) const {
        for (auto it = tokens_.rbegin(); it != tokens_.rend(); ++it) {
            if (it->token == token) {
                out = *it;
                return &ring_[it->slot];
            }
        }
        return nullptr;
    }

    std::string error_text() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

    // The backend drives the ring through these; exposing them keeps the
    // locking discipline in one place instead of friending the backend.
    mutable std::mutex mutex_;
    std::deque<TokenRecord> tokens_;
    std::array<RingSlot, kRingSize> ring_;

    void install(Resources& built) {
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
        device_ = built.device;
        context_ = built.context;
        duplication_ = built.duplication;
        ring_ = built.ring;
        output_rect_px_ = built.output_rect_px;
        rotation_ = built.rotation;
        built.device = nullptr;
        built.context = nullptr;
        built.duplication = nullptr;
        for (RingSlot& slot : built.ring) {
            slot.texture = nullptr;
            slot.keyed_mutex = nullptr;
            slot.shared_handle = nullptr;
        }
        ++generation_;
        ring_cursor_ = 0;
    }

    void release_locked() noexcept {
        release_ring_locked();
        if (duplication_) {
            duplication_->Release();
            duplication_ = nullptr;
        }
        if (context_) {
            context_->Release();
            context_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
        tokens_.clear();
    }

    void release_ring_locked() noexcept {
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
    }

    // The ring is sized from the first frame we actually receive rather than
    // from the mode description, so a rotated or rescaled output cannot leave
    // us with a mismatched CopyResource target.
    void ensure_ring_locked(SizePx size) noexcept {
        if (ring_[0].texture != nullptr && ring_[0].size_px == size) {
            return;
        }
        release_ring_locked();
        for (std::size_t i = 0; i < kRingSize; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(size.width);
            desc.Height = static_cast<UINT>(size.height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags =
                D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

            ID3D11Texture2D* texture = nullptr;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture)) || texture == nullptr) {
                release_ring_locked();
                return;
            }
            IDXGIResource1* resource1 = nullptr;
            if (FAILED(texture->QueryInterface(__uuidof(IDXGIResource1),
                                               reinterpret_cast<void**>(&resource1))) ||
                resource1 == nullptr) {
                texture->Release();
                release_ring_locked();
                return;
            }
            HANDLE handle = nullptr;
            const HRESULT hr = resource1->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
            resource1->Release();
            if (FAILED(hr) || handle == nullptr) {
                texture->Release();
                release_ring_locked();
                return;
            }
            IDXGIKeyedMutex* keyed = nullptr;
            if (FAILED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex),
                                               reinterpret_cast<void**>(&keyed))) ||
                keyed == nullptr) {
                CloseHandle(handle);
                texture->Release();
                release_ring_locked();
                return;
            }
            ring_[i].index = static_cast<std::uint32_t>(i);
            ring_[i].texture = texture;
            ring_[i].keyed_mutex = keyed;
            ring_[i].shared_handle = handle;
            ring_[i].size_px = size;
            ring_[i].token = kInvalidToken;
        }
        ring_cursor_ = 0;
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
                // Waiting on a dead device would flood the bus with errors;
                // recovery only happens when the owner asks for it.
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(kIdleBackoffMs), [this] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           recover_requested_.load(std::memory_order_acquire);
                });
                continue;
            }
            capture_once();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
    }

    void attempt_recovery() noexcept {
        try {
            // DXGI allows one duplication per output per process, so the dead
            // one has to be gone before the replacement is requested; building
            // first would make the retry fail with E_INVALIDARG.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                release_locked();
            }
            Resources built = build_resources(output_id_, requested_rect_px_);
            install(built);
            set_fault(false, std::string{});
            publish(bus_, CaptureRecovered{output_id_});
        } catch (const std::exception& error) {
            set_fault(true, error.what());
            publish(bus_, CaptureLost{output_id_});
            publish(bus_, ErrorReported{kCodeSessionLost, kTextRecoveryFailed});
        } catch (...) {
            set_fault(true, "capture recovery failed");
            publish(bus_, CaptureLost{output_id_});
            publish(bus_, ErrorReported{kCodeSessionLost, kTextRecoveryFailed});
        }
    }

    void capture_once() noexcept {
        if (duplication_ == nullptr) {
            return;
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = duplication_->AcquireNextFrame(kFrameWaitMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return;
        }
        if (FAILED(hr)) {
            // INVALID_CALL only happens when a frame is already held, which
            // this single-consumer loop never does; treat it as transient.
            if (hr == DXGI_ERROR_INVALID_CALL) {
                return;
            }
            handle_lost(hr);
            return;
        }
        if (info.LastPresentTime.QuadPart == 0) {
            // Pointer-only update: the desktop image itself did not change.
            duplication_->ReleaseFrame();
            return;
        }
        copy_and_publish(resource);
        duplication_->ReleaseFrame();
    }

    void handle_lost(HRESULT hr) noexcept {
        const bool removed = (hr == DXGI_ERROR_DEVICE_REMOVED) || (hr == DXGI_ERROR_DEVICE_RESET);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_locked();
            faulted_.store(true, std::memory_order_release);
            last_error_ = "desktop duplication lost: " + hresult_text(hr);
        }
        cv_.notify_all();
        publish(bus_, CaptureLost{output_id_});
        publish(bus_, ErrorReported{removed ? kCodeDeviceRemoved : kCodeSessionLost,
                                    removed ? kTextDeviceRemoved : kTextSessionLost});
    }

    void copy_and_publish(IDXGIResource* resource) noexcept {
        ID3D11Texture2D* source = nullptr;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(&source))) ||
            source == nullptr) {
            return;
        }
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        const SizePx size{static_cast<Px>(source_desc.Width), static_cast<Px>(source_desc.Height)};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_ring_locked(size);
        }

        for (std::size_t attempt = 0; attempt < kRingSize; ++attempt) {
            RingSlot& slot = ring_[ring_cursor_];
            ring_cursor_ = (ring_cursor_ + 1) % kRingSize;
            if (slot.texture == nullptr || slot.keyed_mutex == nullptr) {
                continue;
            }
            // Non-blocking: a slot the renderer still reads is skipped rather
            // than waited on, so capture never stalls behind presentation.
            if (slot.keyed_mutex->AcquireSync(0, 0) != S_OK) {
                continue;
            }
            context_->CopyResource(slot.texture, source);
            slot.keyed_mutex->ReleaseSync(0);
            publish_slot(slot);
            source->Release();
            return;
        }
        source->Release();
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
        frame.desktop_rect_px = output_rect_px_;
        frame.texture_size_px = slot.size_px;
        frame.rotation = rotation_;
        frame.timestamp_qpc = qpc_now();
        frame.frame_index = ++frame_index_;
        frame.valid = true;
        latest_ = frame;
        cv_.notify_all();
    }

    const std::uint32_t output_id_{0};
    const RectPx requested_rect_px_{};
    BoundedEventBus& bus_;

    // Guarded by mutex_, written only by the capture thread.
    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    IDXGIOutputDuplication* duplication_{nullptr};
    std::size_t ring_cursor_{0};
    std::uint64_t generation_{0};
    RectPx output_rect_px_{};
    int rotation_{0};

    std::condition_variable cv_;
    GpuFrame latest_{};
    std::uint64_t frame_index_{0};
    std::atomic<bool> faulted_{false};
    std::string last_error_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> recover_requested_{false};
    std::thread worker_;
};

class DxgiCaptureBackend final : public ICaptureBackend {
public:
    explicit DxgiCaptureBackend(BoundedEventBus& bus) noexcept : bus_(bus) {}
    ~DxgiCaptureBackend() override { stop(); }

    DxgiCaptureBackend(const DxgiCaptureBackend&) = delete;
    DxgiCaptureBackend& operator=(const DxgiCaptureBackend&) = delete;

    void start(std::uint32_t output_id, RectPx desktop_rect_px) override {
        stop();

        std::shared_ptr<DuplicationSession> session;
        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            auto it = g_registry.find(output_id);
            if (it != g_registry.end()) {
                session = it->second.lock();
                if (!session) {
                    g_registry.erase(it);
                }
            }
        }

        if (session) {
            session->start_worker();
        } else {
            auto created = std::shared_ptr<DuplicationSession>(
                new DuplicationSession(output_id, desktop_rect_px, bus_));
            created->initialize();
            created->start_worker();
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_registry[output_id] = created;
            session = std::move(created);
        }

        std::lock_guard<std::mutex> lock(session_mutex_);
        session_ = std::move(session);
        last_seen_index_ = 0;
    }

    std::optional<GpuFrame> acquire(std::uint32_t timeout_ms) override {
        std::shared_ptr<DuplicationSession> session;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session = session_;
        }
        if (!session) {
            throw std::logic_error("DXGI acquire() called before start()");
        }
        return session->wait_frame(last_seen_index_, timeout_ms);
    }

    void stop() noexcept override {
        try {
            release_all_textures();
            std::shared_ptr<DuplicationSession> session;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                session = std::move(session_);
                session_.reset();
                last_seen_index_ = 0;
            }
            // Dropping the last reference joins the capture thread; a session
            // another backend still uses stays alive for that backend.
            session.reset();
        } catch (...) {
        }
    }

    void recover_async() noexcept override {
        try {
            std::shared_ptr<DuplicationSession> session;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                session = session_;
            }
            if (session) {
                session->request_recovery();
            }
        } catch (...) {
        }
    }

    ID3D11Texture2D* resolve_texture(TextureToken token,
                                     ID3D11Device* render_device) noexcept override {
        try {
            if (token == kInvalidToken || render_device == nullptr) {
                return nullptr;
            }
            std::shared_ptr<DuplicationSession> session;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                session = session_;
            }
            if (!session) {
                return nullptr;
            }

            std::lock_guard<std::mutex> session_lock(session->mutex_);
            TokenRecord record{};
            const RingSlot* slot = session->handle_for(token, record);
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
            std::shared_ptr<DuplicationSession> session;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                session = session_;
            }
            if (!session) {
                return;
            }
            std::lock_guard<std::mutex> session_lock(session->mutex_);
            TokenRecord record{};
            if (session->handle_for(token, record) == nullptr) {
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

    const char* backend_name() const noexcept override { return "DXGI Desktop Duplication"; }
    bool is_fallback() const noexcept override { return false; }

    std::string last_error() const override {
        std::shared_ptr<DuplicationSession> session;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session = session_;
        }
        return session ? session->error_text() : std::string{};
    }

private:
    void release_cache_locked() noexcept {
        for (auto& entry : cache_) {
            if (entry.second) {
                entry.second->Release();
            }
        }
        cache_.clear();
        cache_device_ = nullptr;
    }

    BoundedEventBus& bus_;
    mutable std::mutex session_mutex_;
    std::shared_ptr<DuplicationSession> session_;
    std::uint64_t last_seen_index_{0};

    std::mutex cache_mutex_;
    std::unordered_map<std::uint32_t, ID3D11Texture2D*> cache_;
    ID3D11Device* cache_device_{nullptr};
    std::uint64_t cache_generation_{0};
};

}  // namespace

std::unique_ptr<ICaptureBackend> make_dxgi_backend(BoundedEventBus& bus) {
    return std::make_unique<DxgiCaptureBackend>(bus);
}

}  // namespace mag
