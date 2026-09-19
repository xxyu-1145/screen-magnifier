// core/selection_controller.cpp — owns the source selection rectangle.
//
// Every mutation follows the same four steps from the design doc: normalize,
// reject an empty result, clamp against the virtual desktop, re-clamp the
// corner radius, then publish SelectionChanged.
#include "core/selection_controller.h"

#include <limits>
#include <stdexcept>

namespace mag {
namespace {

constexpr Px kPxMin = std::numeric_limits<Px>::min();
constexpr Px kPxMax = std::numeric_limits<Px>::max();

// Saturating adds: a pathological delta can never wrap an edge around int32,
// which would otherwise turn a drag into a rect on the far side of the desktop.
Px add_sat(Px a, Px b) noexcept {
    const std::int64_t sum = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    if (sum > kPxMax) return kPxMax;
    if (sum < kPxMin) return kPxMin;
    return static_cast<Px>(sum);
}

Px sub_sat(Px a, Px b) noexcept {
    const std::int64_t diff = static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b);
    if (diff > kPxMax) return kPxMax;
    if (diff < kPxMin) return kPxMin;
    return static_cast<Px>(diff);
}

// Keeps the part of the rect that is inside the desktop. Used where the edge
// the user is dragging must stop at the desktop border instead of dragging the
// opposite edge along with it.
RectPx clip_to(const RectPx& r, const RectPx& bounds) noexcept {
    return intersect(normalize(r), bounds);
}

}  // namespace

SelectionController::SelectionController(IEventBus& bus, RectPx virtual_bounds_px)
    : bus_(bus), virtual_bounds_px_(normalize(virtual_bounds_px)) {
    if (is_empty(virtual_bounds_px_)) {
        throw std::invalid_argument("SelectionController: virtual bounds must not be empty");
    }
}

void SelectionController::set(const SelectionConfig& sel) {
    const RectPx r = normalize(sel.bounds_px);
    if (is_empty(r)) {
        throw std::invalid_argument("SelectionController::set: selection must not be empty");
    }

    // Clip so a stale config cannot claim desktop area that no longer exists,
    // then slide when the rect landed wholly off-screen.
    RectPx fitted = clip_to(r, virtual_bounds_px_);
    if (is_empty(fitted)) fitted = clamp_into(r, virtual_bounds_px_);

    current_.bounds_px = clamp_into(fitted, virtual_bounds_px_);
    current_.shape = sel.shape;
    current_.corner_radius_px = clamp_corner_radius(sel.corner_radius_px, current_.bounds_px);
    publish_current();
}

void SelectionController::begin(PointPx anchor_px) {
    if (dragging_) {
        throw std::logic_error("SelectionController::begin: a drag is already active");
    }
    drag_start_ = current_;
    anchor_px_ = anchor_px;
    dragging_ = true;
}

void SelectionController::update(PointPx current_px) {
    if (!dragging_) {
        throw std::logic_error("SelectionController::update: no drag in progress");
    }

    const RectPx r = clip_to(rect_from_corners(anchor_px_, current_px), virtual_bounds_px_);
    // A zero-size rect means the pointer has not moved yet; leave the previous
    // selection on screen instead of flashing an empty one.
    if (is_empty(r)) return;

    current_.bounds_px = r;
    current_.corner_radius_px = clamp_corner_radius(current_.corner_radius_px, r);
    publish_current();
}

SelectionConfig SelectionController::commit() {
    if (!dragging_) return current_;

    dragging_ = false;
    // A click without movement leaves an empty working rect; keep the selection
    // the user already had rather than committing a one-pixel spot.
    if (is_empty(current_.bounds_px)) current_ = drag_start_;
    current_.corner_radius_px =
        clamp_corner_radius(current_.corner_radius_px, current_.bounds_px);
    publish_current();
    return current_;
}

void SelectionController::transform(ResizeHandle handle, PointPx delta_px) {
    const bool moving = is_move_handle(handle);

    RectPx c = current_.bounds_px;
    if (moving || handle_has(handle, ResizeHandle::Left)) c.left = add_sat(c.left, delta_px.x);
    if (moving || handle_has(handle, ResizeHandle::Right)) c.right = add_sat(c.right, delta_px.x);
    if (moving || handle_has(handle, ResizeHandle::Top)) c.top = add_sat(c.top, delta_px.y);
    if (moving || handle_has(handle, ResizeHandle::Bottom)) {
        c.bottom = add_sat(c.bottom, delta_px.y);
    }

    c = normalize(c);
    if (width_of(c) < kMinSelectionPx || height_of(c) < kMinSelectionPx) {
        throw std::invalid_argument(
            "SelectionController::transform: delta collapses the selection");
    }

    // A move keeps its size and slides back into view; a resize stops at the
    // desktop edge.
    RectPx clamped = moving ? clamp_into(c, virtual_bounds_px_) : clip_to(c, virtual_bounds_px_);
    if (is_empty(clamped)) clamped = clamp_into(c, virtual_bounds_px_);

    current_.bounds_px = clamped;
    current_.corner_radius_px = clamp_corner_radius(current_.corner_radius_px, clamped);
    publish_current();
}

void SelectionController::move_to(PointPx top_left_px) {
    const Px w = width_of(current_.bounds_px);
    const Px h = height_of(current_.bounds_px);
    if (w < kMinSelectionPx || h < kMinSelectionPx) return;  // nothing to move yet

    const RectPx r{top_left_px.x, top_left_px.y, add_sat(top_left_px.x, w),
                   add_sat(top_left_px.y, h)};
    current_.bounds_px = clamp_into(normalize(r), virtual_bounds_px_);
    current_.corner_radius_px = clamp_corner_radius(current_.corner_radius_px, current_.bounds_px);
    publish_current();
}

void SelectionController::nudge(Px dx, Px dy) {
    const RectPx r{add_sat(current_.bounds_px.left, dx), add_sat(current_.bounds_px.top, dy),
                   add_sat(current_.bounds_px.right, dx), add_sat(current_.bounds_px.bottom, dy)};
    const RectPx& vb = virtual_bounds_px_;
    if (r.left < vb.left || r.right > vb.right || r.top < vb.top || r.bottom > vb.bottom) {
        throw std::out_of_range("SelectionController::nudge: result leaves the virtual desktop");
    }
    current_.bounds_px = r;  // already inside, so no re-clamp and no size change
    publish_current();
}

void SelectionController::grow(Px step_x, Px step_y) {
    const RectPx c = current_.bounds_px;
    const Px left = sub_sat(c.left, step_x);
    const Px right = add_sat(c.right, step_x);
    const Px top = sub_sat(c.top, step_y);
    const Px bottom = add_sat(c.bottom, step_y);

    // A step larger than the selection must be rejected rather than normalized
    // into an inverted rect on the far side of the desktop.
    const std::int64_t w = static_cast<std::int64_t>(right) - left;
    const std::int64_t h = static_cast<std::int64_t>(bottom) - top;
    if (w < kMinSelectionPx || h < kMinSelectionPx) {
        throw std::invalid_argument("SelectionController::grow: step collapses the selection");
    }

    const RectPx grown = clip_to(RectPx{left, top, right, bottom}, virtual_bounds_px_);
    if (is_empty(grown)) {
        throw std::invalid_argument("SelectionController::grow: step collapses the selection");
    }

    current_.bounds_px = grown;
    current_.corner_radius_px = clamp_corner_radius(current_.corner_radius_px, grown);
    publish_current();
}

void SelectionController::set_shape(SelectionShape shape) {
    if (is_empty(current_.bounds_px)) {
        // No geometry to conform yet; record the choice and wait for a rect.
        current_.shape = shape;
        publish_current();
        return;
    }

    SelectionConfig candidate{current_.bounds_px, shape, current_.corner_radius_px};
    candidate = conform_to_shape(candidate);
    if (is_empty(candidate.bounds_px)) {
        throw std::invalid_argument(
            "SelectionController::set_shape: shape is incompatible with the current geometry");
    }

    current_ = candidate;
    publish_current();
}

void SelectionController::set_corner_radius(Px radius) {
    current_.corner_radius_px = clamp_corner_radius(radius, current_.bounds_px);
    publish_current();
}

void SelectionController::set_virtual_bounds(RectPx virtual_bounds_px) {
    const RectPx vb = normalize(virtual_bounds_px);
    if (is_empty(vb)) {
        throw std::invalid_argument("SelectionController::set_virtual_bounds: empty bounds");
    }
    virtual_bounds_px_ = vb;

    // Design doc §3.4: keep the physical-pixel rect unchanged and only slide it
    // back into view, so a monitor change does not silently resize a selection.
    if (is_empty(current_.bounds_px)) return;
    current_.bounds_px = clamp_into(current_.bounds_px, vb);
    current_.corner_radius_px = clamp_corner_radius(current_.corner_radius_px, current_.bounds_px);
    publish_current();
}

void SelectionController::republish() { publish_current(); }

void SelectionController::publish_current() {
    (void)bus_.publish(make_event(SelectionChanged{current_}));
}

}  // namespace mag
