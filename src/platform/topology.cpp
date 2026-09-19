// platform/topology.cpp — monitors, virtual desktop and DPI (§3.4).
//
// Every rectangle reported here is a physical pixel measured from the
// virtual-desktop origin, which may be negative. That only holds while the
// process is PerMonitorV2 aware: without it Win32 hands back virtualised (DIP)
// rectangles and every window we place would drift on a mixed-DPI desktop,
// which is why enable_per_monitor_dpi_v2() exists and must be called first.
#include "platform/topology.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace mag {
namespace {

// DPI is only meaningful inside this band. The design pins it so that a
// nonsense value from a driver or the registry cannot turn into a rectangle
// scaled by a factor of a hundred.
constexpr std::uint32_t kDpiMin = 48;
constexpr std::uint32_t kDpiMax = 768;
// 96 dpi == 100% scaling: the baseline every logical coordinate is stated in.
constexpr std::uint32_t kDpiBaseline = 96;

bool is_per_monitor_v2() noexcept {
    return AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

// floor((v * dpi + 48) / 96), i.e. round-half-up of v * dpi / 96. C++ integer
// division truncates toward zero, so the negative branch needs the explicit
// step down or points left of / above the primary monitor would creep one pixel
// per conversion.
Px scale_logical(Px value, std::uint32_t dpi) noexcept {
    const std::int64_t numerator =
        static_cast<std::int64_t>(value) * static_cast<std::int64_t>(dpi) + 48;
    std::int64_t quotient = numerator / 96;
    if (numerator % 96 != 0 && numerator < 0) --quotient;
    return static_cast<Px>(quotient);
}

// Gap between a point and one axis span of a rectangle; zero inside or on it.
// Rectangles are right/bottom-open, so the last real column is hi - 1.
std::int64_t axis_gap(Px lo, Px hi, Px value) noexcept {
    const std::int64_t lo64 = lo;
    const std::int64_t hi64 = hi;
    const std::int64_t v64 = value;
    if (v64 < lo64) return lo64 - v64;
    const std::int64_t last = hi64 - 1;
    return v64 > last ? v64 - last : 0;
}

// Chebyshev gap: monotone in the true distance and, unlike dx*dx + dy*dy, it
// cannot overflow for the extreme coordinates a virtual desktop may carry.
std::int64_t monitor_gap(const RectPx& rect, PointPx p) noexcept {
    const std::int64_t dx = axis_gap(rect.left, rect.right, p.x);
    const std::int64_t dy = axis_gap(rect.top, rect.bottom, p.y);
    return dx > dy ? dx : dy;
}

struct MonitorEnumContext {
    std::vector<MonitorInfoPx>* monitors;
};

BOOL CALLBACK collect_monitor(HMONITOR handle, HDC, LPRECT, LPARAM param) {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, reinterpret_cast<MONITORINFO*>(&info))) {
        // Abort the whole enumeration instead of returning a half-filled
        // snapshot: a missing monitor would silently place the magnifier on a
        // desktop that does not match the one the user has.
        return FALSE;
    }

    MonitorInfoPx mon;
    mon.monitor_rect_px = RectPx{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right,
                                 info.rcMonitor.bottom};
    mon.work_rect_px =
        RectPx{info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
    mon.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    mon.device_name = info.szDevice;

    UINT dpi_x = kDpiBaseline;
    UINT dpi_y = kDpiBaseline;
    if (SUCCEEDED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
        mon.dpi = dpi_x;
    }
    ctx->monitors->push_back(std::move(mon));
    return TRUE;
}

TopologySnapshot enumerate_topology() {
    std::vector<MonitorInfoPx> monitors;
    MonitorEnumContext ctx{&monitors};
    if (!EnumDisplayMonitors(nullptr, nullptr, &collect_monitor, reinterpret_cast<LPARAM>(&ctx)) ||
        monitors.empty()) {
        throw TopologyQueryError("monitor enumeration failed");
    }

    for (std::size_t i = 0; i < monitors.size(); ++i) {
        // Ordinal ids start at 1 so that the id{0} default stays an "unknown
        // monitor" sentinel and so an id keeps its meaning as long as the
        // enumeration order does.
        monitors[i].id = static_cast<std::uint32_t>(i + 1);
    }

    TopologySnapshot snap;
    snap.virtual_desktop_px = monitors.front().monitor_rect_px;
    for (const MonitorInfoPx& mon : monitors) {
        snap.virtual_desktop_px = unite(snap.virtual_desktop_px, mon.monitor_rect_px);
    }
    snap.monitors = std::move(monitors);
    return snap;
}

bool same_topology(const TopologySnapshot& a, const TopologySnapshot& b) noexcept {
    if (a.virtual_desktop_px != b.virtual_desktop_px) return false;
    if (a.monitors.size() != b.monitors.size()) return false;
    for (std::size_t i = 0; i < a.monitors.size(); ++i) {
        const MonitorInfoPx& x = a.monitors[i];
        const MonitorInfoPx& y = b.monitors[i];
        if (x.id != y.id || x.monitor_rect_px != y.monitor_rect_px ||
            x.work_rect_px != y.work_rect_px || x.dpi != y.dpi || x.primary != y.primary ||
            x.device_name != y.device_name) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Free helpers (§3.4)
// ---------------------------------------------------------------------------

bool enable_per_monitor_dpi_v2() noexcept {
    // Awareness is process-wide and may only be set once: a second call fails
    // with ERROR_ACCESS_DENIED even when the wanted context is already in force,
    // so ask what we hold before asking for it. Returning false for an already
    // correct process would make the caller surface a non-problem.
    if (is_per_monitor_v2()) return true;

    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;

    // Win8.1-era entry point. Per-monitor v1 is not v2 — GetDpiForMonitor still
    // answers correctly but a few messages differ — so say so out loud rather
    // than pretend the request was met in full.
    if (SUCCEEDED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
        OutputDebugStringW(L"magnifier: PerMonitorV2 refused, falling back to PerMonitorV1\n");
        return true;
    }

    // Both entry points refused: report honestly that we are *not* per-monitor
    // aware, which the caller has to surface rather than ignore.
    return is_per_monitor_v2();
}

std::uint32_t dpi_for_point(PointPx p) noexcept {
    const POINT point{p.x, p.y};
    const HMONITOR handle = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (handle == nullptr) return kDpiBaseline;

    UINT dpi_x = kDpiBaseline;
    UINT dpi_y = kDpiBaseline;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0) {
        return kDpiBaseline;  // unknown monitor: 100% is the only safe assumption
    }
    return dpi_x;
}

// ---------------------------------------------------------------------------
// Win32TopologyService
// ---------------------------------------------------------------------------

Win32TopologyService::Win32TopologyService() {
    current_.revision = revision_;
}

TopologySnapshot Win32TopologyService::snapshot() {
    TopologySnapshot fresh = enumerate_topology();
    dirty_ = false;
    // Only a real geometry change deserves a new revision: WM_DISPLAYCHANGE
    // fires for changes that leave the layout identical, and a spurious
    // revision would make the app rebuild capture sessions for nothing.
    if (!current_.monitors.empty() && !same_topology(fresh, current_)) {
        ++revision_;
    }
    fresh.revision = revision_;
    current_ = std::move(fresh);
    return current_;
}

const TopologySnapshot& Win32TopologyService::cached() {
    if (dirty_) (void)snapshot();
    return current_;
}

PointPx Win32TopologyService::logical_to_physical(PointPx logical_px, std::uint32_t dpi) const {
    if (dpi < kDpiMin || dpi > kDpiMax) {
        throw std::invalid_argument("logical_to_physical: dpi outside [48, 768]");
    }
    return PointPx{scale_logical(logical_px.x, dpi), scale_logical(logical_px.y, dpi)};
}

const MonitorInfoPx* TopologySnapshot::monitor_at(PointPx p) const noexcept {
    if (monitors.empty()) return nullptr;
    for (const MonitorInfoPx& mon : monitors) {
        if (contains(mon.monitor_rect_px, p)) return &mon;
    }
    // Monitor rectangles do not tile the plane — an L-shaped or rounded desktop
    // leaves gaps — so fall back to the closest one instead of claiming the
    // point belongs to no display at all.
    const MonitorInfoPx* best = &monitors.front();
    std::int64_t best_gap = monitor_gap(best->monitor_rect_px, p);
    for (const MonitorInfoPx& mon : monitors) {
        const std::int64_t gap = monitor_gap(mon.monitor_rect_px, p);
        if (gap < best_gap) {
            best_gap = gap;
            best = &mon;
        }
    }
    return best;
}

const MonitorInfoPx* TopologySnapshot::primary() const noexcept {
    for (const MonitorInfoPx& mon : monitors) {
        if (mon.primary) return &mon;
    }
    return nullptr;
}

}  // namespace mag
