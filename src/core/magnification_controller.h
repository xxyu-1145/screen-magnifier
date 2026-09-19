// core/magnification_controller.h — owns the zoom factor, the output window
// geometry, and the mapping from source pixels to window pixels.
#pragma once

#include <cstddef>

#include "core/config.h"
#include "core/event_bus.h"
#include "core/types.h"

namespace mag {

// How the source content is placed inside the output window.
//
// The scale is always the configured factor: the window is a viewport onto the
// magnified desktop, not something the picture is stretched to fill. That is
// what keeps the factor box honest -- resizing the window changes how much is
// in view, never how big it is. Everything is uniform, so the picture is never
// distorted.
struct ViewportMapping {
    RectPx dest_rect_px{};      // in client coords, origin (0,0)
    // The source window that fills the client at this scale, relative to the
    // selection's top-left. It is usually *not* the selection: it goes negative
    // when the window reaches past the selection's left/top edge, and wider than
    // the selection when the window shows desktop beside it.
    RectPx src_sub_rect_px{};
    Q16 applied_scale_q16{kQ16One};
    bool valid{false};
};

ViewportMapping compute_viewport(const SelectionConfig& sel,
                                 const MagnificationConfig& mag) noexcept;

// Window size that shows the whole source at exactly `factor`: the shipped
// default relationship between selection size and window size.
SizePx default_output_size(const SelectionConfig& sel, Q16 factor) noexcept;

class MagnificationController {
public:
    // Throws std::invalid_argument when `virtual_bounds_px` is empty.
    MagnificationController(IEventBus& bus, RectPx virtual_bounds_px);

    void set(const MagnificationConfig& cfg);
    void set_factor(Q16 factor_q16);          // throws std::out_of_range
    Q16 select_preset(std::size_t index);     // throws std::out_of_range

    // Supplies the preset table that select_preset() indexes into. Without this
    // the hotkeys and the settings buttons would disagree with the saved values.
    void set_presets(const std::array<Q16, kPresetCount>& presets) noexcept;
    void step_factor(int steps);              // multiplicative ladder, no throw

    void resize_output(SizePx size_px);       // throws std::out_of_range
    void step_output_size(Px dx, Px dy);

    void set_keep_aspect_ratio(bool keep);

    void set_output_position(PointPx top_left_px);
    void center_on(RectPx monitor_rect_px);
    void set_virtual_bounds(RectPx virtual_bounds_px);

    // Recomputes the window size from the current selection and factor.
    // Used when the selection changes while "size follows source" is active.
    void fit_output_to_selection(const SelectionConfig& sel);

    const MagnificationConfig& current() const noexcept { return config_; }
    const PointPx& output_position() const noexcept { return output_position_px_; }
    const RectPx& virtual_bounds() const noexcept { return virtual_bounds_px_; }

    // Total window rect in virtual-desktop physical pixels.
    RectPx output_rect() const noexcept;

    void republish();

private:
    void publish_current();
    void clamp_position();

    IEventBus& bus_;
    RectPx virtual_bounds_px_;
    MagnificationConfig config_{};
    PointPx output_position_px_{100, 100};
    std::array<Q16, kPresetCount> presets_{kQ16One * 2, kQ16One * 4, kQ16One * 8,
                                           kQ16One * 10};
    std::size_t last_preset_{1};
};

// The zoom ladder used by the keyboard +/- path: 1,1.25,1.5,2,2.5,3,4,5,6,8,
// 10,12,16,20. Returns the next value in the requested direction.
Q16 ladder_step(Q16 current, int direction) noexcept;

}  // namespace mag
