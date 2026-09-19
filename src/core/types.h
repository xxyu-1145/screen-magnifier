// core/types.h — fundamental value types.
//
// Contract (design doc §4 "全局技术约束"):
//   * Every coordinate, size, offset and rect bound is a physical-pixel int32.
//   * Rectangles are left-closed / right-open: [left, right) x [top, bottom).
//   * Magnification is a Q16.16 fixed-point integer, never a float.
//   * Nothing in core/ may include a Windows header.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mag {

using Px = std::int32_t;

// Q16.16 fixed point. kOne == 1.0x exactly, so 2x is exactly kOne * 2.
using Q16 = std::uint32_t;
inline constexpr Q16 kQ16One = 65536u;
inline constexpr Q16 kFactorMin = 65536u;    // 1.0x
inline constexpr Q16 kFactorMax = 655360u;   // 10.0x

// Minimum edge length of the output window, per design doc §5.4.
inline constexpr Px kMinOutputEdgePx = 120;

struct PointPx {
    Px x{0};
    Px y{0};

    friend constexpr bool operator==(const PointPx&, const PointPx&) = default;
};

struct SizePx {
    Px width{0};
    Px height{0};

    friend constexpr bool operator==(const SizePx&, const SizePx&) = default;
};

// Half-open in both axes: right is one past the last column, bottom one past
// the last row. width() == right - left without a +1.
struct RectPx {
    Px left{0};
    Px top{0};
    Px right{0};
    Px bottom{0};

    friend constexpr bool operator==(const RectPx&, const RectPx&) = default;
    friend constexpr bool operator!=(const RectPx&, const RectPx&) = default;
};

constexpr Px width_of(const RectPx& r) noexcept { return r.right - r.left; }
constexpr Px height_of(const RectPx& r) noexcept { return r.bottom - r.top; }
constexpr bool is_empty(const RectPx& r) noexcept {
    return r.right <= r.left || r.bottom <= r.top;
}
constexpr Px area_of(const RectPx& r) noexcept {
    return is_empty(r) ? 0 : width_of(r) * height_of(r);
}
constexpr SizePx size_of(const RectPx& r) noexcept {
    return SizePx{width_of(r), height_of(r)};
}

// Forces left <= right and top <= bottom without moving any edge value.
constexpr RectPx normalize(const RectPx& r) noexcept {
    RectPx out = r;
    if (out.left > out.right) {
        const Px t = out.left;
        out.left = out.right;
        out.right = t;
    }
    if (out.top > out.bottom) {
        const Px t = out.top;
        out.top = out.bottom;
        out.bottom = t;
    }
    return out;
}

// Rect built from two opposite corners in any order.
constexpr RectPx rect_from_corners(PointPx a, PointPx b) noexcept {
    return normalize(RectPx{a.x, a.y, b.x, b.y});
}

constexpr bool contains(const RectPx& r, PointPx p) noexcept {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// Empty when the rectangles only touch.
constexpr RectPx intersect(const RectPx& a, const RectPx& b) noexcept {
    RectPx out;
    out.left = a.left > b.left ? a.left : b.left;
    out.top = a.top > b.top ? a.top : b.top;
    out.right = a.right < b.right ? a.right : b.right;
    out.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    if (out.right < out.left) out.right = out.left;
    if (out.bottom < out.top) out.bottom = out.top;
    return out;
}

constexpr bool intersects(const RectPx& a, const RectPx& b) noexcept {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// Smallest rectangle containing both.
constexpr RectPx unite(const RectPx& a, const RectPx& b) noexcept {
    if (is_empty(a)) return b;
    if (is_empty(b)) return a;
    RectPx out;
    out.left = a.left < b.left ? a.left : b.left;
    out.top = a.top < b.top ? a.top : b.top;
    out.right = a.right > b.right ? a.right : b.right;
    out.bottom = a.bottom > b.bottom ? a.bottom : b.bottom;
    return out;
}

// Slides r back inside bounds without resizing; a rect larger than bounds is
// aligned to the origin edge so the result stays as visible as possible.
constexpr RectPx clamp_into(const RectPx& r, const RectPx& bounds) noexcept {
    RectPx out = r;
    const Px w = width_of(out);
    const Px h = height_of(out);
    const Px bound_w = width_of(bounds);
    const Px bound_h = height_of(bounds);

    if (w >= bound_w) {
        out.left = bounds.left;
    } else if (out.left < bounds.left) {
        out.left = bounds.left;
    } else if (out.right > bounds.right) {
        out.left = bounds.right - w;
    }
    if (h >= bound_h) {
        out.top = bounds.top;
    } else if (out.top < bounds.top) {
        out.top = bounds.top;
    } else if (out.bottom > bounds.bottom) {
        out.top = bounds.bottom - h;
    }
    out.right = out.left + w;
    out.bottom = out.top + h;
    return out;
}

// Integer-only resize of every edge by a delta, then normalize.
// handle uses the ResizeHandle bit values below so callers can pass a mask.
enum class SelectionShape { Rectangle, Circle, Ellipse, RoundedRectangle };

enum class ResizeHandle : std::uint32_t {
    None = 0,
    Left = 1u << 0,
    Top = 1u << 1,
    Right = 1u << 2,
    Bottom = 1u << 3,
    TopLeft = Left | Top,
    TopRight = Right | Top,
    BottomLeft = Left | Bottom,
    BottomRight = Right | Bottom,
    Move = 1u << 4,
};

constexpr std::uint32_t handle_bits(ResizeHandle h) noexcept {
    return static_cast<std::uint32_t>(h);
}
constexpr bool handle_has(ResizeHandle h, ResizeHandle part) noexcept {
    return (handle_bits(h) & handle_bits(part)) != 0;
}
constexpr bool is_move_handle(ResizeHandle h) noexcept {
    return handle_bits(h) == handle_bits(ResizeHandle::Move);
}

enum class InteractionState { Off, Interactive, PassThrough, EdgeArmed, Suspended };

constexpr const char* to_string(InteractionState s) noexcept {
    switch (s) {
        case InteractionState::Off: return "Off";
        case InteractionState::Interactive: return "Interactive";
        case InteractionState::PassThrough: return "PassThrough";
        case InteractionState::EdgeArmed: return "EdgeArmed";
        case InteractionState::Suspended: return "Suspended";
    }
    return "?";
}

constexpr const char* to_string(SelectionShape s) noexcept {
    switch (s) {
        case SelectionShape::Rectangle: return "Rectangle";
        case SelectionShape::Circle: return "Circle";
        case SelectionShape::Ellipse: return "Ellipse";
        case SelectionShape::RoundedRectangle: return "RoundedRectangle";
    }
    return "?";
}

struct SelectionConfig {
    RectPx bounds_px{};
    SelectionShape shape{SelectionShape::Rectangle};
    // Clamped to [0, min(width,height)/2] by every mutation path.
    Px corner_radius_px{0};

    friend constexpr bool operator==(const SelectionConfig&, const SelectionConfig&) = default;
};

struct MagnificationConfig {
    Q16 factor_q16{kQ16One};
    SizePx output_size_px{320, 240};
    // When true the source is letterboxed inside the window instead of being
    // stretched, so extreme window aspect ratios never distort the content.
    bool keep_aspect_ratio{true};

    friend constexpr bool operator==(const MagnificationConfig&, const MagnificationConfig&) = default;
};

struct RenderSnapshot {
    SelectionConfig selection{};
    MagnificationConfig magnification{};
    InteractionState interaction_state{InteractionState::Off};
    std::uint64_t revision{0};

    friend constexpr bool operator==(const RenderSnapshot&, const RenderSnapshot&) = default;
};

// ---------------------------------------------------------------------------
// Fixed-point helpers. All integer math; no float ever touches a coordinate.
// ---------------------------------------------------------------------------

// Exact for every value in the supported range; used for display and presets.
constexpr double q16_to_double(Q16 q) noexcept {
    return static_cast<double>(q) / 65536.0;
}

// Nearest Q16.16 for a decimal factor, e.g. 2.5 -> 163840.
constexpr Q16 q16_from_double(double v) noexcept {
    return static_cast<Q16>(v * 65536.0 + 0.5);
}

// Multiplies a pixel count by a Q16.16 factor with round-half-up, using int64
// so a 20x factor times a 16384-px edge cannot overflow.
constexpr Px scale_px(Px value, Q16 factor) noexcept {
    const std::int64_t prod = static_cast<std::int64_t>(value) * static_cast<std::int64_t>(factor);
    return static_cast<Px>((prod + 32768) / 65536);
}

// ---------------------------------------------------------------------------
// Exceptions (design doc §5.1). These are part of the contract: boundary layers
// convert them to error events but must not swallow them.
// ---------------------------------------------------------------------------

struct CaptureUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct DeviceRemoved : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct RenderInitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct TopologyQueryError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class RenderExitReason { StopRequested, DeviceRemoved, FatalError };

}  // namespace mag
