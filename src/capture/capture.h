// capture/capture.h — the capture backend contract.
//
// Design doc §5.5: a backend never hands out a bare texture pointer across a
// thread boundary. It hands out an immutable token; only the thread that owns
// the rendering device resolves that token into a texture.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/event_bus.h"
#include "core/types.h"

namespace mag {

// An immutable handle into the backend's resource table.
using TextureToken = std::uint64_t;
inline constexpr TextureToken kInvalidToken = 0;

struct GpuFrame {
    TextureToken texture_token{kInvalidToken};
    std::uint32_t output_id{0};
    // Where this texture sits in virtual-desktop physical pixels.
    RectPx desktop_rect_px{};
    SizePx texture_size_px{};
    // Degrees clockwise: 0, 90, 180 or 270.
    //
    // Desktop Duplication hands back the surface in the display's *native*
    // orientation, which for a pivoted monitor is not the orientation of the
    // desktop. A portrait screen therefore arrives rotated, and sampling it
    // with desktop-space coordinates would read the wrong pixels entirely.
    int rotation{0};
    std::uint64_t timestamp_qpc{0};
    // Monotonic counter so the consumer can tell a repeat from a new frame.
    std::uint64_t frame_index{0};
    bool valid{false};
};

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    // Starts a duplication session for one output. Throws CaptureUnavailable
    // when the output cannot be duplicated.
    virtual void start(std::uint32_t output_id, RectPx desktop_rect_px) = 0;

    // Waits up to timeout_ms for the newest frame. Returns std::nullopt when
    // the desktop did not change. Throws DeviceRemoved when the session died.
    virtual std::optional<GpuFrame> acquire(std::uint32_t timeout_ms) = 0;

    // Stops the session. Must not throw.
    virtual void stop() noexcept = 0;

    // Non-throwing rebuild after device removal.
    virtual void recover_async() noexcept = 0;

    // Called on the entity that owns `render_device`. Opens and caches the
    // shared texture behind `token`. Returns nullptr when the token is stale.
    virtual ID3D11Texture2D* resolve_texture(TextureToken token,
                                             ID3D11Device* render_device) noexcept = 0;

    // Drops cached render-side views of a token. Safe to call for any token.
    virtual void retire_texture(TextureToken token) noexcept = 0;

    // Releases every cached view. Call before destroying the render device.
    virtual void release_all_textures() noexcept = 0;

    // Human-readable backend name for the status UI ("DXGI Desktop Duplication"
    // or "GDI BitBlt").
    virtual const char* backend_name() const noexcept = 0;

    // True when the backend is a last-resort CPU path with lower performance.
    virtual bool is_fallback() const noexcept = 0;

    // Last failure detail, for the "capture unavailable" banner.
    virtual std::string last_error() const = 0;
};

// Creates a DXGI Desktop Duplication backend. Falls back to GDI BitBlt capture
// when duplication is refused for every output.
//
// `bus` receives CaptureLost / CaptureRecovered / ErrorReported events.
struct BackendSelection {
    std::unique_ptr<ICaptureBackend> backend;
    bool used_fallback{false};
    std::string note;
};

BackendSelection create_capture_backend(BoundedEventBus& bus);

// Individual factories, also used by the tests.
std::unique_ptr<ICaptureBackend> make_dxgi_backend(BoundedEventBus& bus);
std::unique_ptr<ICaptureBackend> make_gdi_backend(BoundedEventBus& bus);

}  // namespace mag
