// render/render_service.cpp — D3D11 + DirectComposition presentation.
//
// Pipeline per frame: for every monitor that intersects the visible part of
// the source region, draw one quad that maps that monitor's sub-rectangle onto
// the right place in the output window, sampling that monitor's captured
// texture. The pixel shader then applies the selection mask and writes
// premultiplied alpha, which DirectComposition blends over the desktop.
//
// Drawing one quad per monitor (rather than one quad for the window) is what
// makes a selection that spans several screens work without any CPU-side
// stitching: each screen contributes its own tile, and the tiles abut exactly
// because adjacent edges are derived from the same integer source coordinate.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "render/render_service.h"

#include <dcomp.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "core/hit_test.h"
#include "core/magnification_controller.h"

namespace mag {

// Older Windows SDK headers predate this value; the OS itself has supported it
// since Windows 10 2004 and returns an error on anything earlier.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ---------------------------------------------------------------------------
// Runtime shader compilation.
//
// The zig toolchain ships no d3dcompiler import library, and d3dcompiler_47.dll
// is present on every Windows 10/11 system, so resolve the entry point at
// runtime instead of linking it.
// ---------------------------------------------------------------------------

namespace {

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn g_d3d_compile = nullptr;

}  // namespace

bool load_d3dcompiler() noexcept {
    if (g_d3d_compile) return true;
    HMODULE mod = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) mod = ::LoadLibraryW(L"d3dcompiler.dll");
    if (!mod) return false;
    g_d3d_compile = reinterpret_cast<D3DCompileFn>(::GetProcAddress(mod, "D3DCompile"));
    if (!g_d3d_compile) {
        ::FreeLibrary(mod);
        return false;
    }
    return true;
}

namespace shaders {

// Shape type ids shared with the C++ side.
constexpr float kShapeRectangle = 0.0f;
constexpr float kShapeCircle = 1.0f;
constexpr float kShapeEllipse = 2.0f;
constexpr float kShapeRounded = 3.0f;

const char* magnifier_hlsl() noexcept {
    return R"HLSL(
// b0: geometry for a single monitor tile plus the global mask description.
cbuffer Params : register(b0)
{
    float4 dest_rect;     // tile destination in client pixels: l, t, r, b
    // UV of the tile's top-left corner (xy) and its change per destination
    // pixel in x (zw). A rotated display makes u advance with the destination's
    // y, which a single min/max UV rectangle cannot express.
    float4 uv_origin;
    float4 uv_dy;         // change in UV per destination pixel in y (xy)
    float4 mask_rect;     // mask (shape bounds) in client pixels: l, t, r, b
    float4 shape_params;  // x=shape id, y=corner radius px, z=border px, w=bicubic flag
    float4 viewport_size; // x=client width, y=client height
    float4 style;         // rgb=border colour, a=border enabled
};

Texture2D    g_source : register(t0);
SamplerState g_sampler : register(s0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// A single quad expanded from SV_VertexID, so no vertex buffer is needed.
VSOut VS(uint id : SV_VertexID)
{
    float2 corner = float2((float)(id & 1u), (float)((id >> 1u) & 1u));
    float2 px = lerp(dest_rect.xy, dest_rect.zw, corner);

    VSOut o;
    o.pos = float4(px.x / viewport_size.x * 2.0 - 1.0,
                   1.0 - px.y / viewport_size.y * 2.0,
                   0.0, 1.0);
    o.uv = uv_origin.xy + corner.x * uv_origin.zw + corner.y * uv_dy.xy;
    return o;
}

// Coverage of the selection shape at client-space pixel position p.
// Returns 1 inside the shape and falls smoothly to 0 across roughly one pixel,
// which is what keeps circle and rounded-rect edges from looking jagged.
float ShapeCoverage(float2 p, float4 r, float4 sp)
{
    float2 centre = (r.xy + r.zw) * 0.5;
    // Not named `half`: that is a built-in HLSL type keyword.
    float2 ext    = max((r.zw - r.xy) * 0.5, float2(0.5, 0.5));
    float2 d      = abs(p - centre);
    float  type   = sp.x;
    // One client pixel expressed in the shape's normalised radius space.
    float  aa     = 1.0 / max(min(ext.x, ext.y), 1.0);

    if (type < 0.5)                       // rectangle
        return 1.0;

    if (type < 1.5)                       // circle: min(w,h) diameter, centred
    {
        float radius = min(ext.x, ext.y);
        float dist   = length(d);
        return 1.0 - smoothstep(radius - 1.0, radius, dist);
    }

    if (type < 2.5)                       // ellipse
    {
        float k = length(d / ext);
        return 1.0 - smoothstep(1.0 - aa * 2.0, 1.0, k);
    }

    // Rounded rectangle: straight bands plus four corner quadrants.
    float radius = clamp(sp.y, 0.0, min(ext.x, ext.y));
    float2 q     = max(d - (ext - radius), 0.0);
    float dist   = length(q) - radius;
    return 1.0 - smoothstep(-1.0, 0.0, dist);
}

// Five-tap smoothing: more detail than a single bilinear fetch without the cost
// of nine discrete samples. The tap offset comes from the screen-space
// derivative of the UV, so it tracks the true texel footprint at any zoom.
float3 SampleSmoothed(float2 uv)
{
    float2 texel = max(abs(ddx(uv)), abs(ddy(uv))) * 0.5;
    float3 c = g_source.Sample(g_sampler, uv).rgb;
    float3 n = g_source.Sample(g_sampler, uv + float2(0.0, -texel.y)).rgb;
    float3 s = g_source.Sample(g_sampler, uv + float2(0.0,  texel.y)).rgb;
    float3 w = g_source.Sample(g_sampler, uv + float2(-texel.x, 0.0)).rgb;
    float3 e = g_source.Sample(g_sampler, uv + float2( texel.x, 0.0)).rgb;
    return c * 0.4 + (n + s + w + e) * 0.15;
}

float4 PS(VSOut i) : SV_Target
{
    float coverage = ShapeCoverage(i.pos.xy, mask_rect, shape_params);
    if (coverage <= 0.0005)
        discard;

    float3 rgb = g_source.Sample(g_sampler, i.uv).rgb;
    if (shape_params.w > 0.5)
        rgb = SampleSmoothed(i.uv);

    // A thin accent outline traces the mask edge so the user can see where the
    // magnified region ends. It is only visible when borders are enabled.
    float edge = 1.0 - coverage;
    if (style.a > 0.5 && edge > 0.002)
        rgb = lerp(rgb, style.rgb, saturate(edge * 1.6));

    // Premultiplied alpha, matching DXGI_ALPHA_MODE_PREMULTIPLIED.
    return float4(rgb * coverage, coverage);
}
)HLSL";
}

}  // namespace shaders

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

namespace {

// cbuffer layout; must match the HLSL declaration byte for byte.
struct alignas(16) ShaderParams {
    float dest_rect[4];
    float uv_origin[4];
    float uv_dy[4];
    float mask_rect[4];
    float shape_params[4];
    float viewport_size[4];
    float style[4];
};

// D3DCompile's own words, which is the only way a shader error is actionable.
std::string blob_text(ID3DBlob* blob) {
    if (!blob || !blob->GetBufferPointer()) return {};
    return std::string(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
}

// Desktop pixel -> texture UV.
//
// Desktop Duplication returns the surface in the display's native orientation,
// so on a pivoted monitor the texture's axes are swapped relative to the
// desktop. Without this the magnifier would sample a rotated copy of the screen
// and show the wrong region entirely -- a defect that is invisible on a
// landscape display and total on a portrait one.
void desktop_to_uv(const RectPx& monitor, int rotation, Px dx, Px dy, float& u, float& v) noexcept {
    const float nx = static_cast<float>(dx - monitor.left) /
                     static_cast<float>(std::max(1, width_of(monitor)));
    const float ny = static_cast<float>(dy - monitor.top) /
                     static_cast<float>(std::max(1, height_of(monitor)));
    switch (rotation) {
        case 90:
            u = ny;
            v = 1.0f - nx;
            break;
        case 180:
            u = 1.0f - nx;
            v = 1.0f - ny;
            break;
        case 270:
            u = 1.0f - ny;
            v = nx;
            break;
        default:
            u = nx;
            v = ny;
            break;
    }
}

// Rounds a rational pixel mapping the same way on both sides of a shared edge,
// which is what keeps adjacent monitor tiles seam-free.
inline Px map_edge(Px src, Px src_origin, Px src_span, Px dst_origin, Px dst_span) noexcept {
    if (src_span <= 0) return dst_origin;
    const std::int64_t num = static_cast<std::int64_t>(src - src_origin) * dst_span;
    return dst_origin + static_cast<Px>((num + src_span / 2) / src_span);
}

}  // namespace

struct RenderService::Impl {
    Config cfg{};
    SnapshotMailbox* mailbox{nullptr};

    std::atomic<bool> stop_requested{false};
    std::atomic<int> pending_client_w{0};
    std::atomic<int> pending_client_h{0};
    std::atomic<int> target_fps{60};
    std::atomic<int> filter{static_cast<int>(ScaleFilter::Auto)};

    // D3D objects. All touched only by the render thread.
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* context{nullptr};
    IDXGISwapChain1* swapchain{nullptr};
    IDCompositionDevice* dcomp_device{nullptr};
    IDCompositionTarget* dcomp_target{nullptr};
    IDCompositionVisual* dcomp_visual{nullptr};
    ID3D11RenderTargetView* rtv{nullptr};
    ID3D11VertexShader* vs{nullptr};
    ID3D11PixelShader* ps_point{nullptr};
    ID3D11PixelShader* ps_bilinear{nullptr};
    ID3D11SamplerState* sampler_point{nullptr};
    ID3D11SamplerState* sampler_linear{nullptr};
    ID3D11Buffer* cbuffer{nullptr};
    ID3D11RasterizerState* raster{nullptr};
    ID3D11BlendState* blend{nullptr};

    // GPU timing, so the reported figure is a measurement rather than a guess.
    ID3D11Query* disjoint_query{nullptr};
    ID3D11Query* ts_begin{nullptr};
    ID3D11Query* ts_end{nullptr};
    bool query_pending{false};
    bool timer_open{false};
    int over_budget_frames{0};
    std::chrono::steady_clock::time_point last_pace_{};

    // Reads back the previous frame's GPU time and degrades the filter when the
    // budget is persistently exceeded (design doc §3.2).
    void poll_gpu_time() {
        if (!query_pending || !disjoint_query || !context) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (context->GetData(disjoint_query, &disjoint, sizeof(disjoint), 0) != S_OK) return;
        query_pending = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0) return;

        UINT64 t0 = 0;
        UINT64 t1 = 0;
        if (context->GetData(ts_begin, &t0, sizeof(t0), 0) != S_OK) return;
        if (context->GetData(ts_end, &t1, sizeof(t1), 0) != S_OK) return;
        if (t1 <= t0) return;

        stats.gpu_frame_ms =
            static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(disjoint.Frequency);

        if (stats.gpu_frame_ms > 8.0) {
            if (++over_budget_frames >= 3) {
                over_budget_frames = 0;
                const int current = filter.load();
                if (current == static_cast<int>(ScaleFilter::Auto) ||
                    current == static_cast<int>(ScaleFilter::Bicubic)) {
                    filter.store(static_cast<int>(ScaleFilter::Bilinear));
                    stats.filter_downgraded = true;
                }
            }
        } else {
            over_budget_frames = 0;
        }
    }

    void begin_gpu_timer() {
        if (query_pending || !disjoint_query || !ts_begin || !ts_end) return;
        context->Begin(disjoint_query);
        context->End(ts_begin);
        query_pending = true;
        timer_open = true;
    }

    void end_gpu_timer() {
        // Only close a region this frame actually opened: a stray End() would
        // be matched against the next frame's Begin() and corrupt the reading.
        if (!query_pending) return;
        if (!timer_open) return;
        context->End(ts_end);
        context->End(disjoint_query);
        timer_open = false;
    }

    UINT swap_w{0};
    UINT swap_h{0};
    UINT client_w{0};
    UINT client_h{0};

    // Set when the composition target has been rebuilt and the window's display
    // affinity still has to be re-asserted once a frame has gone out. See the
    // comment in create_composition().
    bool reassert_affinity{false};

    // Frame bookkeeping.
    //
    // A texture handed out by resolve_texture() stays owned by the capture
    // backend's cache; the renderer only holds a borrowed pointer. Releasing it
    // here would drop a reference the cache still believes it owns and the next
    // cache flush would double-free it.
    std::vector<ID3D11Texture2D*> tile_textures;   // borrowed, never released
    std::vector<IDXGIKeyedMutex*> tile_mutexes;    // borrowed from the texture
    std::vector<std::uint64_t> tile_tokens;
    std::vector<RectPx> tile_rects;
    std::vector<int> tile_rotations;
    std::vector<ICaptureBackend*> tile_owners;

    // `stats` is owned by the render thread and written freely. `published`
    // is the copy the UI thread reads, refreshed once per loop iteration under
    // the mutex -- so the reader never observes a half-updated struct and the
    // writer never pays for a lock per counter.
    mutable std::mutex stats_mutex;
    RenderStats stats{};
    RenderStats published{};
    std::string adapter_desc;

    void publish_stats() noexcept {
        std::lock_guard<std::mutex> lock(stats_mutex);
        published = stats;
    }
    std::string last_shader_error;
    std::chrono::steady_clock::time_point fps_window_start{};
    std::uint64_t fps_window_frames{0};

    // --- lifecycle ---------------------------------------------------------

    HRESULT create_device() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // BGRA support is mandatory for DirectComposition.
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                         2, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) {
            // A machine with no usable GPU still gets a working magnifier, just
            // a slower one; the status bar reports the adapter either way.
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                                     D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        if (FAILED(hr)) return hr;

        // Record the adapter name for the settings window.
        IDXGIDevice* dxgi_device = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                             reinterpret_cast<void**>(&dxgi_device)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    char narrow[256]{};
                    ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrow,
                                          sizeof(narrow) - 1, nullptr, nullptr);
                    adapter_desc = narrow;
                    const double mb = static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0);
                    char suffix[64];
                    std::snprintf(suffix, sizeof(suffix), " (%.0f MB VRAM)", mb);
                    adapter_desc += suffix;
                }
                adapter->Release();
            }
            dxgi_device->Release();
        }
        return S_OK;
    }

    HRESULT create_pipeline() {
        HRESULT hr = S_OK;

        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* ps_blob = nullptr;
        ID3DBlob* error_blob = nullptr;
        if (!g_d3d_compile) return E_FAIL;

        hr = g_d3d_compile(shaders::magnifier_hlsl(), std::strlen(shaders::magnifier_hlsl()),
                           "magnifier.hlsl", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vs_blob,
                           &error_blob);
        if (FAILED(hr)) {
            last_shader_error = "vertex shader: " + blob_text(error_blob);
            if (error_blob) error_blob->Release();
            return hr;
        }
        hr = g_d3d_compile(shaders::magnifier_hlsl(), std::strlen(shaders::magnifier_hlsl()),
                           "magnifier.hlsl", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &ps_blob,
                           &error_blob);
        if (FAILED(hr)) {
            last_shader_error = "pixel shader: " + blob_text(error_blob);
            vs_blob->Release();
            if (error_blob) error_blob->Release();
            return hr;
        }

        device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                   &vs);
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                  &ps_point);
        // Smoothing is selected by a flag in the constant buffer rather than by
        // a second shader object, so one pixel shader serves all three filters.
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                  &ps_bilinear);

        vs_blob->Release();
        ps_blob->Release();
        if (error_blob) error_blob->Release();

        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        device->CreateSamplerState(&sd, &sampler_point);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        device->CreateSamplerState(&sd, &sampler_linear);

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ShaderParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &cbuffer);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, &raster);

        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        // DirectComposition expects premultiplied source colour.
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bld, &blend);

        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&qd, &disjoint_query);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, &ts_begin);
        device->CreateQuery(&qd, &ts_end);

        return (vs && ps_bilinear && cbuffer && sampler_linear) ? S_OK : E_FAIL;
    }

    HRESULT create_composition() {
        IDXGIDevice* dxgi_device = nullptr;
        HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                            reinterpret_cast<void**>(&dxgi_device));
        if (FAILED(hr)) return hr;

        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_device->GetAdapter(&adapter);
        if (FAILED(hr)) {
            dxgi_device->Release();
            return hr;
        }
        IDXGIFactory2* factory = nullptr;
        hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
        adapter->Release();
        if (FAILED(hr)) {
            dxgi_device->Release();
            return hr;
        }

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = client_w;
        scd.Height = client_h;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.Stereo = FALSE;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // Three buffers: two is enough for correctness, but with a flip model
        // the third is what stops Present from throttling the loop to every
        // other vblank while the compositor still holds the front buffer.
        scd.BufferCount = 3;
        scd.Scaling = DXGI_SCALING_STRETCH;
        // FLIP_SEQUENTIAL is the only flip model a composition swap chain
        // accepts, and it is what lets the desktop compositor blend our alpha.
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        hr = factory->CreateSwapChainForComposition(device, &scd, nullptr, &swapchain);
        factory->Release();
        if (FAILED(hr)) {
            dxgi_device->Release();
            return hr;
        }

        // DCompositionCreateDevice needs the DXGI device alive; releasing it
        // first would hand the compositor a dangling pointer.
        hr = ::DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice),
                                        reinterpret_cast<void**>(&dcomp_device));
        dxgi_device->Release();
        if (FAILED(hr)) return hr;

        hr = dcomp_device->CreateTargetForHwnd(cfg.hwnd, TRUE, &dcomp_target);
        if (FAILED(hr)) return hr;
        hr = dcomp_device->CreateVisual(&dcomp_visual);
        if (FAILED(hr)) return hr;
        hr = dcomp_visual->SetContent(swapchain);
        if (FAILED(hr)) return hr;
        hr = dcomp_target->SetRoot(dcomp_visual);
        if (FAILED(hr)) return hr;

        // A window's display affinity does not survive a teardown of its
        // composition target. The renderer drops the target and swap chain when
        // the magnifier is switched off, so on the way back the window still
        // reads as WDA_EXCLUDEFROMCAPTURE from GetWindowDisplayAffinity yet
        // composites opaque black and is no longer excluded from capture, and
        // it stays that way until the affinity changes again -- re-applying the
        // same value does nothing, because DWM only reacts to a transition.
        // Clearing it here and letting the loop re-assert the configured value
        // once a frame has gone out is that transition.
        //
        // Only when the exclusion is actually in use: touching the affinity of a
        // window that never had one is not free, and setting WDA_NONE on a
        // composition window that was never excluded is itself enough to leave
        // it not compositing.
        if (cfg.exclude_from_capture) {
            SetWindowDisplayAffinity(cfg.hwnd, WDA_NONE);
            reassert_affinity = true;
        }
        swap_w = client_w;
        swap_h = client_h;
        return create_render_target();
    }

    HRESULT create_render_target() {
        if (rtv) {
            rtv->Release();
            rtv = nullptr;
        }
        ID3D11Texture2D* back = nullptr;
        HRESULT hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&back));
        if (FAILED(hr)) return hr;
        hr = device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
        return hr;
    }

    void release_all() noexcept {
        // Hand the textures back through their owner before dropping the
        // device, so the backend's cache never outlives the device that opened
        // it. The texture pointers themselves are borrowed and must not be
        // released here.
        for (auto* backend : cfg.captures) {
            if (backend) backend->release_all_textures();
        }
        for (auto* mutex : tile_mutexes) {
            if (mutex) mutex->Release();
        }
        tile_mutexes.clear();
        tile_textures.clear();
        tile_tokens.clear();
        tile_rects.clear();
        tile_rotations.clear();
        tile_owners.clear();

        auto rel = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };
        rel(rtv);
        rel(blend);
        rel(raster);
        rel(cbuffer);
        rel(disjoint_query);
        rel(ts_begin);
        rel(ts_end);
        query_pending = false;
        rel(sampler_linear);
        rel(sampler_point);
        rel(ps_bilinear);
        rel(ps_point);
        rel(vs);
        rel(dcomp_visual);
        rel(dcomp_target);
        rel(dcomp_device);
        rel(swapchain);
        rel(context);
        rel(device);
    }

    // --- per-frame ---------------------------------------------------------

    void apply_pending_resize() {
        const int w = pending_client_w.exchange(0);
        const int h = pending_client_h.exchange(0);
        if (w > 0 && h > 0) {
            // Record the size even when there is no swap chain yet: the window
            // may have been resized while the magnifier was off, and the next
            // create_composition() has to use the current size, not the one
            // captured at startup.
            client_w = static_cast<UINT>(w);
            client_h = static_cast<UINT>(h);
        }
        if (!swapchain || client_w == 0 || client_h == 0) return;
        if (client_w == swap_w && client_h == swap_h) return;

        if (rtv) {
            rtv->Release();
            rtv = nullptr;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->Flush();

        const HRESULT hr = swapchain->ResizeBuffers(0, client_w, client_h,
                                                    DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr) && SUCCEEDED(create_render_target())) {
            swap_w = client_w;
            swap_h = client_h;
        } else {
            // Leaving swap_w/swap_h stale makes the next frame retry the resize
            // instead of rendering into a target that was never created.
            stats.device_resets++;
        }
    }

    ID3D11PixelShader* pick_pixel_shader(Q16 factor, bool& bicubic) const {
        bicubic = false;
        switch (static_cast<ScaleFilter>(filter.load())) {
            case ScaleFilter::Point:
                return ps_point;
            case ScaleFilter::Bilinear:
                return ps_bilinear;
            case ScaleFilter::Bicubic:
                bicubic = true;
                return ps_bilinear;
            case ScaleFilter::Auto:
            default:
                // Integer factors land exactly on texel centres, so nearest
                // neighbour is both crisp and cheapest; everything else gets
                // bilinear. This is the rule from the design document.
                if (factor != 0 && (factor % kQ16One) == 0) return ps_point;
                return ps_bilinear;
        }
    }

    ID3D11SamplerState* pick_sampler(Q16 factor) const {
        switch (static_cast<ScaleFilter>(filter.load())) {
            case ScaleFilter::Point:
                return sampler_point;
            case ScaleFilter::Bilinear:
            case ScaleFilter::Bicubic:
                return sampler_linear;
            case ScaleFilter::Auto:
            default:
                if (factor != 0 && (factor % kQ16One) == 0) return sampler_point;
                return sampler_linear;
        }
    }

    void render_frame(const RenderSnapshot& snap) {
        if (!rtv || client_w == 0 || client_h == 0) return;
        poll_gpu_time();

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(client_w);
        vp.Height = static_cast<float>(client_h);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->ClearRenderTargetView(rtv, clear);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        begin_gpu_timer();

        const auto vp_mapping = compute_viewport(snap.selection, snap.magnification);
        if (!vp_mapping.valid) {
            context->Flush();
            return;
        }

        // The visible source region in virtual-desktop coordinates.
        const RectPx sel = snap.selection.bounds_px;
        const RectPx visible_src{
            sel.left + vp_mapping.src_sub_rect_px.left,
            sel.top + vp_mapping.src_sub_rect_px.top,
            sel.left + vp_mapping.src_sub_rect_px.right,
            sel.top + vp_mapping.src_sub_rect_px.bottom,
        };
        const RectPx dest = vp_mapping.dest_rect_px;
        if (is_empty(visible_src) || is_empty(dest)) return;

        // Where the selection shape lands in client space. The viewport may show
        // desktop outside the selection, and the mask has to cover that too --
        // otherwise a rectangle selection would leave its own window transparent
        // around the edges -- so it is grown to the client rect around the
        // selection's centre. A window smaller than the selection keeps the
        // selection's own extent, which is what makes it crop rather than clip
        // the shape: a circle stays a circle and the window is simply inside it.
        const Q16 scale = vp_mapping.applied_scale_q16;
        const Px sel_w = scale_px(width_of(sel), scale);
        const Px sel_h = scale_px(height_of(sel), scale);
        const Px sel_left = dest.left - scale_px(vp_mapping.src_sub_rect_px.left, scale);
        const Px sel_top = dest.top - scale_px(vp_mapping.src_sub_rect_px.top, scale);
        const Px mask_w = std::max<Px>(sel_w, width_of(dest));
        const Px mask_h = std::max<Px>(sel_h, height_of(dest));
        const Px mask_left = sel_left + sel_w / 2 - mask_w / 2;
        const Px mask_top = sel_top + sel_h / 2 - mask_h / 2;
        const RectPx mask_rect{mask_left, mask_top, mask_left + mask_w, mask_top + mask_h};

        bool bicubic = false;
        ID3D11PixelShader* ps = pick_pixel_shader(snap.magnification.factor_q16, bicubic);
        ID3D11SamplerState* sampler = pick_sampler(snap.magnification.factor_q16);
        if (!ps || !sampler) return;

        float shape_id = shaders::kShapeRectangle;
        switch (snap.selection.shape) {
            case SelectionShape::Rectangle: shape_id = shaders::kShapeRectangle; break;
            case SelectionShape::Circle: shape_id = shaders::kShapeCircle; break;
            case SelectionShape::Ellipse: shape_id = shaders::kShapeEllipse; break;
            case SelectionShape::RoundedRectangle: shape_id = shaders::kShapeRounded; break;
        }
        const float corner_px = static_cast<float>(scale_px(snap.selection.corner_radius_px, scale));

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetSamplers(0, 1, &sampler);
        context->RSSetState(raster);
        const float blend_factor[4] = {0, 0, 0, 0};
        context->OMSetBlendState(blend, blend_factor, 0xffffffff);
        context->VSSetConstantBuffers(0, 1, &cbuffer);
        context->PSSetConstantBuffers(0, 1, &cbuffer);

        const bool interactive = snap.interaction_state != InteractionState::Off;

        std::size_t drawn = 0;
        for (std::size_t i = 0; i < tile_rects.size(); ++i) {
            if (!tile_textures[i]) continue;

            // Skip rather than wait: a frame that is still being copied is not
            // worth stalling the render loop for, and the next iteration will
            // pick it up.
            IDXGIKeyedMutex* mutex =
                (i < tile_mutexes.size()) ? tile_mutexes[i] : nullptr;
            if (mutex && mutex->AcquireSync(0, 0) != S_OK) continue;

            const RectPx over = intersect(visible_src, tile_rects[i]);
            if (is_empty(over)) {
                if (mutex) mutex->ReleaseSync(0);
                continue;
            }

            // Map the monitor's overlapping part into both client space and UV.
            const RectPx tile_dest{
                map_edge(over.left, visible_src.left, width_of(visible_src), dest.left,
                         width_of(dest)),
                map_edge(over.top, visible_src.top, height_of(visible_src), dest.top,
                         height_of(dest)),
                map_edge(over.right, visible_src.left, width_of(visible_src), dest.left,
                         width_of(dest)),
                map_edge(over.bottom, visible_src.top, height_of(visible_src), dest.top,
                         height_of(dest)),
            };
            if (is_empty(tile_dest)) {
                if (mutex) mutex->ReleaseSync(0);
                continue;
            }

            const RectPx mon = tile_rects[i];
            if (width_of(mon) <= 0 || height_of(mon) <= 0) {
                if (mutex) mutex->ReleaseSync(0);
                continue;
            }
            // Three corners of the tile: the top-left gives the origin, the
            // other two give how the UV advances across and down the
            // destination. That is what makes a quarter-turned display come out
            // the right way up instead of transposed.
            const int rotation = (i < tile_rotations.size()) ? tile_rotations[i] : 0;
            float u0 = 0.0f;
            float v0 = 0.0f;
            float ux = 0.0f;
            float vx = 0.0f;
            float uy = 0.0f;
            float vy = 0.0f;
            desktop_to_uv(mon, rotation, over.left, over.top, u0, v0);
            desktop_to_uv(mon, rotation, over.right, over.top, ux, vx);
            desktop_to_uv(mon, rotation, over.left, over.bottom, uy, vy);

            ShaderParams params{};
            params.dest_rect[0] = static_cast<float>(tile_dest.left);
            params.dest_rect[1] = static_cast<float>(tile_dest.top);
            params.dest_rect[2] = static_cast<float>(tile_dest.right);
            params.dest_rect[3] = static_cast<float>(tile_dest.bottom);
            params.uv_origin[0] = u0;
            params.uv_origin[1] = v0;
            params.uv_origin[2] = ux - u0;
            params.uv_origin[3] = vx - v0;
            params.uv_dy[0] = uy - u0;
            params.uv_dy[1] = vy - v0;
            params.mask_rect[0] = static_cast<float>(mask_rect.left);
            params.mask_rect[1] = static_cast<float>(mask_rect.top);
            params.mask_rect[2] = static_cast<float>(mask_rect.right);
            params.mask_rect[3] = static_cast<float>(mask_rect.bottom);
            params.shape_params[0] = shape_id;
            params.shape_params[1] = corner_px;
            params.shape_params[2] = static_cast<float>(cfg.show_border ? 2 : 0);
            params.shape_params[3] = bicubic ? 1.0f : 0.0f;
            params.viewport_size[0] = static_cast<float>(client_w);
            params.viewport_size[1] = static_cast<float>(client_h);
            params.viewport_size[2] = 1.0f / static_cast<float>(client_w);
            params.viewport_size[3] = 1.0f / static_cast<float>(client_h);
            params.style[0] = 0.18f;
            params.style[1] = 0.62f;
            params.style[2] = 1.00f;
            params.style[3] = (cfg.show_border && interactive) ? 1.0f : 0.0f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &params, sizeof(params));
                context->Unmap(cbuffer, 0);
            }

            ID3D11ShaderResourceView* srv = nullptr;
            if (SUCCEEDED(device->CreateShaderResourceView(tile_textures[i], nullptr, &srv))) {
                context->PSSetShaderResources(0, 1, &srv);
                context->Draw(4, 0);
                ID3D11ShaderResourceView* none = nullptr;
                context->PSSetShaderResources(0, 1, &none);
                srv->Release();
            }
            if (mutex) mutex->ReleaseSync(0);
            ++drawn;
        }
        end_gpu_timer();
        if (drawn == 0) stats.frames_skipped++;
    }

    void present() {
        if (!swapchain) return;
        // Do not wait on vsync here: the loop paces itself, and blocking in
        // Present on a composition swap chain costs a whole extra vblank, which
        // is what halves the achievable frame rate.
        const HRESULT hr = swapchain->Present(0, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            stats.device_resets++;
            throw DeviceRemoved("swap chain device removed");
        }
        if (hr == DXGI_STATUS_OCCLUDED) {
            // Completely covered: nothing to show, so stop burning frames.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }
        stats.frames_presented++;
    }

    void update_fps() {
        const auto now = std::chrono::steady_clock::now();
        ++fps_window_frames;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - fps_window_start)
                                 .count();
        if (elapsed >= 500) {
            stats.present_fps = static_cast<double>(fps_window_frames) * 1000.0 /
                                static_cast<double>(elapsed);
            fps_window_frames = 0;
            fps_window_start = now;
        }
    }

    void pace() {
        const int fps = std::max(15, target_fps.load());
        const auto frame_budget = std::chrono::microseconds(1000000 / fps);
        const auto now = std::chrono::steady_clock::now();
        if (last_pace_.time_since_epoch().count() != 0) {
            const auto spent = now - last_pace_;
            const auto remaining = frame_budget - spent;
            // Sleeping for a sliver is counter-productive: the scheduler
            // overshoots a sub-millisecond request by milliseconds, which costs
            // more frames than the sleep could ever save.
            if (remaining > std::chrono::microseconds(1500))
                std::this_thread::sleep_for(remaining);
        }
        last_pace_ = std::chrono::steady_clock::now();
    }
};

RenderService::RenderService(const Config& cfg, SnapshotMailbox& mailbox)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = cfg;
    impl_->mailbox = &mailbox;
    impl_->target_fps = cfg.target_fps;
    impl_->filter = static_cast<int>(cfg.filter);
    impl_->fps_window_start = std::chrono::steady_clock::now();
}

RenderService::~RenderService() {
    impl_->release_all();
}

void RenderService::submit_snapshot(const RenderSnapshot& snapshot) noexcept {
    impl_->mailbox->submit(snapshot);
}

void RenderService::request_stop() noexcept {
    impl_->stop_requested = true;
}

void RenderService::notify_client_size(SizePx size_px) noexcept {
    impl_->pending_client_w = size_px.width;
    impl_->pending_client_h = size_px.height;
}

void RenderService::set_target_fps(int fps) noexcept {
    impl_->target_fps = fps;
}

void RenderService::set_filter(ScaleFilter filter) noexcept {
    impl_->filter = static_cast<int>(filter);
}

void RenderService::set_exclude_from_capture(bool enable) noexcept {
    impl_->cfg.exclude_from_capture = enable;
    // Also apply it now, so flipping the setting is visible immediately rather
    // than only at the next time the composition is rebuilt.
    if (impl_->cfg.hwnd != nullptr) {
        SetWindowDisplayAffinity(impl_->cfg.hwnd,
                                 enable ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    }
}

RenderStats RenderService::stats() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    return impl_->published;
}

std::string RenderService::adapter_description() const {
    return impl_->adapter_desc;
}

RenderExitReason RenderService::run(std::stop_token stop_token) {
    if (!g_d3d_compile) load_d3dcompiler();

    RECT client{};
    ::GetClientRect(impl_->cfg.hwnd, &client);
    impl_->client_w = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    impl_->client_h = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));

    // The device is created on the first frame that actually needs it, and torn
    // down again whenever the magnifier is switched off. Holding a D3D11 device
    // and a composition swap chain while idle would cost tens of megabytes of
    // driver working set for nothing, which is exactly the idle budget the
    // design sets (and its own Off transition says "stop presenting and release
    // the session").
    auto ensure_initialized = [this]() -> const char* {
        if (impl_->device) return nullptr;
        if (!g_d3d_compile && !load_d3dcompiler()) {
            return "d3dcompiler_47.dll or its D3DCompile entry point is unavailable";
        }
        if (FAILED(impl_->create_device())) return "D3D11CreateDevice failed";
        if (FAILED(impl_->create_pipeline())) {
            static std::string detail;
            detail = impl_->last_shader_error.empty() ? std::string("no compiler diagnostic")
                                                      : impl_->last_shader_error;
            impl_->release_all();
            static std::string message;
            message = "shader compilation failed: " + detail;
            return message.c_str();
        }
        if (FAILED(impl_->create_composition())) {
            impl_->release_all();
            return "DirectComposition setup failed";
        }
        impl_->dcomp_device->Commit();
        return nullptr;
    };

    // The capture backends publish an opaque token per output. Resolve each
    // token once per output and keep the texture for reuse.
    std::uint64_t last_seq = 0;
    RenderSnapshot snap{};
    bool have_snapshot = false;
    int consecutive_failures = 0;

    while (!stop_token.stop_requested() && !impl_->stop_requested.load()) {
        try {
            ++impl_->stats.loop_ticks;
            SnapshotMailbox::Entry entry{};
            if (impl_->mailbox->take_latest(entry, last_seq)) {
                snap = entry.snapshot;
                have_snapshot = true;
                impl_->stats.consumed_sequence = entry.sequence;
                impl_->stats.consumed_state =
                    static_cast<std::uint32_t>(snap.interaction_state);
            }

            impl_->apply_pending_resize();

            // Nothing to show while the magnifier is off: release the whole GPU
            // session and idle at a low rate, so the process costs effectively
            // no memory or CPU when it is not in use.
            if (!have_snapshot || snap.interaction_state == InteractionState::Off) {
                if (impl_->device) impl_->release_all();
                impl_->stats.stage = RenderStats::StageIdle;
                impl_->publish_stats();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (const char* failure = ensure_initialized()) {
                impl_->release_all();
                impl_->stats.stage = RenderStats::StageInitFailed;
                throw RenderInitError(failure);
            }
            impl_->stats.stage = RenderStats::StageInitialized;

            // Pull the newest frame from every configured capture backend.
            {
                for (auto* backend : impl_->cfg.captures) {
                    if (!backend) continue;
                    std::optional<GpuFrame> frame;
                    try {
                        // Zero timeout: never stall the render loop waiting for
                        // a desktop that is not changing.
                        frame = backend->acquire(0);
                    } catch (const DeviceRemoved&) {
                        continue;
                    } catch (const std::exception&) {
                        // A session that has been stopped or is still being
                        // rebuilt rejects the call outright. The render thread
                        // must survive that: ending it here would leave the
                        // magnifier permanently blank after one hide.
                        impl_->stats.capture_timeouts++;
                        continue;
                    }
                    if (!frame || !frame->valid) {
                        impl_->stats.capture_timeouts++;
                        continue;
                    }
                    const std::uint32_t idx = frame->output_id;
                    if (idx >= impl_->tile_textures.size()) {
                        impl_->tile_textures.resize(idx + 1, nullptr);
                        impl_->tile_mutexes.resize(idx + 1, nullptr);
                        impl_->tile_tokens.resize(idx + 1, kInvalidToken);
                        impl_->tile_rects.resize(idx + 1, RectPx{});
                        impl_->tile_rotations.resize(idx + 1, 0);
                        impl_->tile_owners.resize(idx + 1, nullptr);
                    }
                    if (impl_->tile_tokens[idx] != frame->texture_token) {
                        // Retire through the owner: the backend's cache may
                        // still be holding this token's texture.
                        if (impl_->tile_owners[idx] &&
                            impl_->tile_tokens[idx] != kInvalidToken) {
                            impl_->tile_owners[idx]->retire_texture(impl_->tile_tokens[idx]);
                        }
                        if (impl_->tile_mutexes[idx]) {
                            impl_->tile_mutexes[idx]->Release();
                            impl_->tile_mutexes[idx] = nullptr;
                        }
                        impl_->tile_textures[idx] =
                            backend->resolve_texture(frame->texture_token, impl_->device);
                        impl_->tile_tokens[idx] = frame->texture_token;
                        impl_->tile_rects[idx] = frame->desktop_rect_px;
                        impl_->tile_rotations[idx] = frame->rotation;
                        impl_->tile_owners[idx] = backend;
                        if (impl_->tile_textures[idx]) {
                            // The capture side writes through a keyed mutex, so
                            // the renderer has to observe it too or it would be
                            // sampling a texture mid-copy.
                            IDXGIKeyedMutex* mutex = nullptr;
                            if (SUCCEEDED(impl_->tile_textures[idx]->QueryInterface(
                                    __uuidof(IDXGIKeyedMutex),
                                    reinterpret_cast<void**>(&mutex)))) {
                                impl_->tile_mutexes[idx] = mutex;
                            }
                        }
                    }
                }
            }

            impl_->stats.stage = RenderStats::StageCaptured;
            impl_->render_frame(snap);
            impl_->stats.stage = RenderStats::StageRendered;
            impl_->present();
            impl_->stats.stage = RenderStats::StagePresented;
            // Second half of the affinity transition described in
            // create_composition(): restore the configured value now that the
            // rebuilt target is actually presenting a frame.
            if (impl_->reassert_affinity) {
                impl_->reassert_affinity = false;
                SetWindowDisplayAffinity(
                    impl_->cfg.hwnd,
                    impl_->cfg.exclude_from_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
            }
            impl_->update_fps();
            impl_->pace();
            impl_->publish_stats();
            consecutive_failures = 0;
        } catch (const DeviceRemoved&) {
            impl_->stats.device_resets++;
            ++consecutive_failures;
            if (consecutive_failures > 3) {
                impl_->release_all();
                return RenderExitReason::DeviceRemoved;
            }
            impl_->release_all();
            if (FAILED(impl_->create_device()) || FAILED(impl_->create_pipeline()) ||
                FAILED(impl_->create_composition())) {
                impl_->release_all();
                return RenderExitReason::DeviceRemoved;
            }
            impl_->dcomp_device->Commit();
        }
    }

    impl_->release_all();
    return RenderExitReason::StopRequested;
}

}  // namespace mag
