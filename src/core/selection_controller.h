// core/selection_controller.h — owns the source selection rectangle.
//
// Pure domain object: it publishes value events and never touches Win32.
#pragma once

#include <cstdint>
#include <optional>

#include "core/event_bus.h"
#include "core/hit_test.h"
#include "core/types.h"

namespace mag {

class SelectionController {
public:
    // `virtual_bounds_px` is the union of all monitors in physical pixels.
    // Throws std::invalid_argument when the bounds are empty.
    SelectionController(IEventBus& bus, RectPx virtual_bounds_px);

    // Sets the whole selection at once (config load, picking a region).
    // Throws std::invalid_argument when the result would be empty.
    void set(const SelectionConfig& sel);

    // Begins a drag-out. Throws std::logic_error if a drag is already active.
    void begin(PointPx anchor_px);

    // Moves the working rect to (anchor, current). Throws std::logic_error
    // when no drag is in progress.
    void update(PointPx current_px);

    // Ends a drag. Returns the committed selection. Also fires on a click with
    // no movement, in which case the existing selection is preserved.
    SelectionConfig commit();

    bool dragging() const noexcept { return dragging_; }

    // Drag an existing edge / corner / whole selection.
    // Throws std::invalid_argument when the delta would collapse the rect.
    void transform(ResizeHandle handle, PointPx delta_px);

    // Absolute move that keeps the size and slides back inside the desktop.
    void move_to(PointPx top_left_px);

    // Pixel-level adjustment, dx/dy in {-1,0,1} per axis.
    // Throws std::out_of_range when the result would leave the desktop.
    void nudge(Px dx, Px dy);

    // Grow/shrink every edge by `step_px` (negative shrinks) and by `dy` for
    // the vertical axis. Used by the keyboard resize path.
    void grow(Px step_x, Px step_y);

    void set_shape(SelectionShape shape);
    void set_corner_radius(Px radius);

    // Re-clamps against a new desktop size (monitor topology change).
    void set_virtual_bounds(RectPx virtual_bounds_px);

    const SelectionConfig& current() const noexcept { return current_; }
    const RectPx& virtual_bounds() const noexcept { return virtual_bounds_px_; }

    // Publishes the current value as SelectionChanged.
    void republish();

private:
    void publish_current();

    IEventBus& bus_;
    RectPx virtual_bounds_px_;
    SelectionConfig current_{};
    SelectionConfig drag_start_{};
    PointPx anchor_px_{};
    bool dragging_{false};
};

}  // namespace mag
