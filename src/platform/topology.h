// platform/topology.h — monitors, virtual desktop and DPI, all in physical
// pixels (design doc §3.4).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"

namespace mag {

struct MonitorInfoPx {
    std::uint32_t id{0};  // stable within a session; ordinal-based
    RectPx monitor_rect_px{};
    RectPx work_rect_px{};
    std::uint32_t dpi{96};
    bool primary{false};
    std::wstring device_name;
};

struct TopologySnapshot {
    RectPx virtual_desktop_px{};
    std::vector<MonitorInfoPx> monitors;
    std::uint64_t revision{0};

    const MonitorInfoPx* monitor_at(PointPx p) const noexcept;
    const MonitorInfoPx* primary() const noexcept;
};

class ITopologyService {
public:
    virtual ~ITopologyService() = default;

    // Rebuilds and returns the current topology. Coordinates are physical
    // pixels relative to the virtual-desktop origin (which may be negative).
    virtual TopologySnapshot snapshot() = 0;

    // Converts a logical (96-dpi-normalised) coordinate to physical pixels
    // using the documented floor((v*dpi + 48)/96) rounding.
    // Throws std::invalid_argument when dpi is outside [48, 768].
    virtual PointPx logical_to_physical(PointPx logical_px, std::uint32_t dpi) const = 0;
};

class Win32TopologyService final : public ITopologyService {
public:
    Win32TopologyService();

    TopologySnapshot snapshot() override;
    PointPx logical_to_physical(PointPx logical_px, std::uint32_t dpi) const override;

    // Cached copy of the last snapshot() call, refreshed on demand.
    const TopologySnapshot& cached();

    // Called from the window proc on WM_DISPLAYCHANGE / WM_DPICHANGED.
    void invalidate() noexcept { dirty_ = true; }

    std::uint64_t revision() const noexcept { return revision_; }

private:
    TopologySnapshot current_{};
    std::uint64_t revision_{1};
    bool dirty_{true};
};

// Enables PerMonitorV2 awareness for the process. Returns false when the OS
// refused, which the caller must surface rather than ignore.
bool enable_per_monitor_dpi_v2() noexcept;

// DPI of the monitor that contains `p`, defaulting to 96 when unknown.
std::uint32_t dpi_for_point(PointPx p) noexcept;

}  // namespace mag
