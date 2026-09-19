// core/hit_test.h — integer-only geometry for the four selection shapes.
//
// Everything here is exact integer arithmetic (design doc §3.1): no float is
// allowed to touch a coordinate, because a float round-trip is what produces
// the off-by-one drift the acceptance criteria forbid.
#pragma once

#include "core/types.h"

namespace mag {

// Width of the grab band, in source pixels, around a selection edge.
inline constexpr Px kHandleBandPx = 5;
// Smallest selection the user may drag out.
inline constexpr Px kMinSelectionPx = 1;

// Is `p` inside the shape described by `sel`?
//
// Rectangle: half-open bounds test.
// Circle:    the rect is squared off to min(w,h) first, per the design doc.
// Ellipse:   dx^2*ry^2 + dy^2*rx^2 <= rx^2*ry^2.
// Rounded:   centre bands plus four corner quadrants.
bool point_in_shape(const SelectionConfig& sel, PointPx p) noexcept;

// Which resize handle (if any) is under `p`, given a grab band of
// `band_px` source pixels. Returns ResizeHandle::None when the point is
// outside the shape entirely.
ResizeHandle handle_at(const SelectionConfig& sel, PointPx p, Px band_px = kHandleBandPx) noexcept;

// The visual bounds of the shape. A circle is centred inside the selection
// rect and uses min(w,h) as its diameter, so the drawn shape and the hit
// region can never disagree.
RectPx shape_bounds(const SelectionConfig& sel) noexcept;

// Normalises a corner radius into [0, min(w,h)/2] as the design requires.
Px clamp_corner_radius(Px radius, const RectPx& bounds) noexcept;

// Applies the geometry rules implied by a shape change: a circle equalises
// its axes to a centred square. The corner radius is preserved so that
// switching away from a rounded rectangle and back does not lose it.
SelectionConfig conform_to_shape(SelectionConfig sel) noexcept;

}  // namespace mag
