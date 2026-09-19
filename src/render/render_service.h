// render/render_service.h — the D3D11 presentation path.
//
// Crop -> Scale -> Mask -> Composite, presented through a DirectComposition
// swap chain so the masked-away region is genuinely transparent and the whole
// path stays on the GPU (design doc §3.2).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "capture/capture.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/types.h"

namespace mag {

struct RenderStats {
    double present_fps{0.0};
    double gpu_frame_ms{0.0};
    std::uint64_t frames_presented{0};
    std::uint64_t frames_skipped{0};
    std::uint64_t capture_timeouts{0};
    std::uint64_t device_resets{0};
    bool filter_downgraded{false};
    // How far the last iteration of the render loop got. A stalled loop is
    // otherwise indistinguishable from a desktop that simply is not changing,
    // and the two need completely different fixes.
    enum Stage : std::uint32_t {
        StageStarting = 0,
        StageIdle,        // magnifier off, GPU session released
        StageInitialized, // device and composition are up
        StageCaptured,    // a frame was pulled from every backend
        StageRendered,    // draw calls issued
        StagePresented,   // swap chain presented
        StageInitFailed,  // initialization refused; see the reported error
    };
    std::uint32_t stage{StageStarting};
    // Heartbeat: proves the render thread is still looping even when every
    // iteration takes the idle path.
    std::uint64_t loop_ticks{0};
    // Mailbox sequence the loop last consumed, and the state it carried. A
    // renderer stuck on a stale snapshot is otherwise indistinguishable from a
    // stopped one.
    std::uint64_t consumed_sequence{0};
    std::uint32_t consumed_state{99};
};

class IRenderService {
public:
    virtual ~IRenderService() = default;

    // Stores the newest immutable snapshot. Never throws; old snapshots are
    // simply overwritten.
    virtual void submit_snapshot(const RenderSnapshot& snapshot) noexcept = 0;

    // Blocks until stop is requested. Throws RenderInitError when D3D or
    // DirectComposition cannot be created at all.
    virtual RenderExitReason run(std::stop_token stop_token) = 0;
};

class RenderService final : public IRenderService {
public:
    struct Config {
        HWND hwnd{nullptr};
        RectPx virtual_desktop_px{};
        // One backend per monitor. A selection that spans screens is served by
        // drawing one tile per backend, so cross-screen magnification needs no
        // CPU-side stitching.
        std::vector<ICaptureBackend*> captures;
        int target_fps{60};
        ScaleFilter filter{ScaleFilter::Auto};
        bool show_border{true};
        // Whether the composed window carries WDA_EXCLUDEFROMCAPTURE. The
        // renderer re-asserts it whenever it rebuilds the composition target,
        // because DWM does not keep a window's display affinity across that.
        bool exclude_from_capture{true};
    };

    RenderService(const Config& cfg, SnapshotMailbox& mailbox);
    ~RenderService() override;

    RenderService(const RenderService&) = delete;
    RenderService& operator=(const RenderService&) = delete;

    void submit_snapshot(const RenderSnapshot& snapshot) noexcept override;
    RenderExitReason run(std::stop_token stop_token) override;

    // Safe to call from any thread.
    void request_stop() noexcept;

    // The window's client area changed; the swap chain is resized on the
    // render thread at the next frame.
    void notify_client_size(SizePx size_px) noexcept;

    void set_target_fps(int fps) noexcept;
    void set_filter(ScaleFilter filter) noexcept;
    void set_exclude_from_capture(bool enable) noexcept;

    RenderStats stats() const noexcept;
    std::string adapter_description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The HLSL used by the pipeline, compiled at runtime with D3DCompile.
namespace shaders {
const char* magnifier_hlsl() noexcept;
}  // namespace shaders

// Loads D3DCompile out of d3dcompiler_47.dll. Returns false when the DLL or
// the entry point is missing, in which case the renderer falls back to a
// precompiled pixel-shader path.
bool load_d3dcompiler() noexcept;

}  // namespace mag
