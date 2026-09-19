// core/hit_test.cpp — exact integer geometry for the four selection shapes.
//
// Every test below is a comparison of int64 products. Coordinates stay int32;
// widening happens only for the intermediate squares, so a 20x capture of a
// large desktop cannot overflow the way a naive int32 version would.
#include "core/hit_test.h"

#include <cstdint>

namespace mag {
namespace {

std::int64_t sq(std::int64_t v) noexcept { return v * v; }

std::int64_t iabs(std::int64_t v) noexcept { return v < 0 ? -v : v; }

// Gap between the point and each half-open edge of `b`. Computed in int64 so a
// point far outside the desktop cannot wrap the subtraction.
struct EdgeGap {
    std::int64_t left;
    std::int64_t right;
    std::int64_t top;
    std::int64_t bottom;
};

EdgeGap edge_gap(const RectPx& b, PointPx p) noexcept {
    return EdgeGap{static_cast<std::int64_t>(p.x) - b.left,
                   static_cast<std::int64_t>(b.right) - 1 - p.x,
                   static_cast<std::int64_t>(p.y) - b.top,
                   static_cast<std::int64_t>(b.bottom) - 1 - p.y};
}

// A shape narrower than twice the band has both opposing edges "near" at once;
// the nearer one wins so the result is always a single valid handle.
void add_horizontal(std::uint32_t& mask, EdgeGap g, Px band_px) noexcept {
    const bool near_left = g.left < band_px;
    const bool near_right = g.right < band_px;
    if (near_left && near_right) {
        mask |= handle_bits(g.left <= g.right ? ResizeHandle::Left : ResizeHandle::Right);
    } else if (near_left) {
        mask |= handle_bits(ResizeHandle::Left);
    } else if (near_right) {
        mask |= handle_bits(ResizeHandle::Right);
    }
}

void add_vertical(std::uint32_t& mask, EdgeGap g, Px band_px) noexcept {
    const bool near_top = g.top < band_px;
    const bool near_bottom = g.bottom < band_px;
    if (near_top && near_bottom) {
        mask |= handle_bits(g.top <= g.bottom ? ResizeHandle::Top : ResizeHandle::Bottom);
    } else if (near_top) {
        mask |= handle_bits(ResizeHandle::Top);
    } else if (near_bottom) {
        mask |= handle_bits(ResizeHandle::Bottom);
    }
}

}  // namespace

bool point_in_shape(const SelectionConfig& sel, PointPx p) noexcept {
    const RectPx b = normalize(sel.bounds_px);
    if (is_empty(b)) return false;

    switch (sel.shape) {
        case SelectionShape::Rectangle:
            return contains(b, p);

        case SelectionShape::Circle: {
            // The rect is squared to min(w,h) and centred, so the drawn circle
            // and the hit region can never disagree.
            const RectPx box = shape_bounds(sel);
            const Px radius = width_of(box) / 2;
            if (radius <= 0) return contains(box, p);
            const std::int64_t dx = static_cast<std::int64_t>(p.x) - (box.left + radius);
            const std::int64_t dy = static_cast<std::int64_t>(p.y) - (box.top + radius);
            if (iabs(dx) > radius || iabs(dy) > radius) return false;
            return sq(dx) + sq(dy) <= sq(radius);
        }

        case SelectionShape::Ellipse: {
            const Px rx = width_of(b) / 2;
            const Px ry = height_of(b) / 2;
            const std::int64_t dx = static_cast<std::int64_t>(p.x) - (b.left + rx);
            const std::int64_t dy = static_cast<std::int64_t>(p.y) - (b.top + ry);
            // The guard also bounds dx/dy by the half-axes, which is what keeps
            // the products below exact while rx*ry stays under ~2^31 — orders of
            // magnitude beyond any physical-pixel virtual desktop.
            if (iabs(dx) > rx || iabs(dy) > ry) return false;
            // With a half-axis of zero the non-degenerate branch would accept
            // the whole strip; the true shape is the one-pixel segment.
            if (rx == 0 || ry == 0) return true;
            const std::int64_t rx2 = sq(rx);
            const std::int64_t ry2 = sq(ry);
            return sq(dx) * ry2 + sq(dy) * rx2 <= rx2 * ry2;
        }

        case SelectionShape::RoundedRectangle: {
            if (!contains(b, p)) return false;
            const Px radius = clamp_corner_radius(sel.corner_radius_px, b);
            if (radius <= 0) return true;
            const Px rx = width_of(b) / 2;
            const Px ry = height_of(b) / 2;
            const std::int64_t dx = iabs(static_cast<std::int64_t>(p.x) - (b.left + rx));
            const std::int64_t dy = iabs(static_cast<std::int64_t>(p.y) - (b.top + ry));
            const std::int64_t inner_x = rx - radius;
            const std::int64_t inner_y = ry - radius;
            // Central bands are always inside; only the four corner quadrants
            // need the circle test.
            if (dx <= inner_x || dy <= inner_y) return true;
            return sq(dx - inner_x) + sq(dy - inner_y) <= sq(radius);
        }
    }
    return false;
}

ResizeHandle handle_at(const SelectionConfig& sel, PointPx p, Px band_px) noexcept {
    if (!point_in_shape(sel, p)) return ResizeHandle::None;
    if (band_px <= 0) return ResizeHandle::Move;

    const EdgeGap g = edge_gap(shape_bounds(sel), p);
    std::uint32_t mask = 0;
    add_horizontal(mask, g, band_px);
    add_vertical(mask, g, band_px);
    if (mask == 0) return ResizeHandle::Move;
    return static_cast<ResizeHandle>(mask);
}

RectPx shape_bounds(const SelectionConfig& sel) noexcept {
    const RectPx b = normalize(sel.bounds_px);
    if (sel.shape != SelectionShape::Circle) return b;

    const Px w = width_of(b);
    const Px h = height_of(b);
    const Px side = w < h ? w : h;
    if (side <= 0) return RectPx{b.left, b.top, b.left, b.top};
    // Odd leftovers land on the right/bottom edge rather than being split.
    const Px left = b.left + (w - side) / 2;
    const Px top = b.top + (h - side) / 2;
    return RectPx{left, top, left + side, top + side};
}

Px clamp_corner_radius(Px radius, const RectPx& bounds) noexcept {
    const RectPx b = normalize(bounds);
    const Px w = width_of(b);
    const Px h = height_of(b);
    const Px limit = (w < h ? w : h) / 2;
    if (radius <= 0 || limit <= 0) return 0;
    return radius > limit ? limit : radius;
}

SelectionConfig conform_to_shape(SelectionConfig sel) noexcept {
    sel.bounds_px = normalize(sel.bounds_px);
    if (sel.shape == SelectionShape::Circle) {
        sel.bounds_px = shape_bounds(sel);
    }
    // The corner radius is deliberately preserved across shape changes: zeroing
    // it for a rectangle would mean switching to Rounded and back left a
    // "rounded" selection that was indistinguishable from a plain rectangle.
    // A rectangle simply ignores the value.
    sel.corner_radius_px = clamp_corner_radius(sel.corner_radius_px, sel.bounds_px);
    return sel;
}

}  // namespace mag
