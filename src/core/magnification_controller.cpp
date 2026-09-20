// core/magnification_controller.cpp — zoom factor, output window geometry and
// the source-to-window viewport mapping (design doc §3.2 and §5.4).
#include "core/magnification_controller.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mag {
namespace {

constexpr Px kPxMax = std::numeric_limits<Px>::max();
constexpr Px kPxMin = std::numeric_limits<Px>::min();

constexpr Px add_sat(Px a, Px b) noexcept {
    const std::int64_t r = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    if (r > static_cast<std::int64_t>(kPxMax)) return kPxMax;
    if (r < static_cast<std::int64_t>(kPxMin)) return kPxMin;
    return static_cast<Px>(r);
}

constexpr Px clamp_px(Px v, Px lo, Px hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

constexpr Q16 clamp_q16(Q16 v, Q16 lo, Q16 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

constexpr Px largest(Px a, Px b) noexcept { return a > b ? a : b; }

// Where a window of `size` has to start to sit over the middle of `rect`.
//
// Every resize goes through this. Taking the corner instead -- which is what
// leaving the position alone amounts to -- walks the window across the screen as
// it grows, and the slider that resizes it would drag it away from whatever the
// user was looking at.
constexpr PointPx centred_top_left(RectPx rect, SizePx size) noexcept {
    const RectPx r = normalize(rect);
    return PointPx{r.left + (width_of(r) - size.width) / 2,
                   r.top + (height_of(r) - size.height) / 2};
}

// The keyboard zoom ladder: 1, 1.25, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10 written as
// exact Q16.16 words. It stops at kFactorMax so the top of the ladder and the
// top of the supported range cannot disagree.
constexpr Q16 kLadder[] = {
    65536u,    // 1.0
    81920u,    // 1.25
    98304u,    // 1.5
    131072u,   // 2.0
    163840u,   // 2.5
    196608u,   // 3.0
    262144u,   // 4.0
    327680u,   // 5.0
    393216u,   // 6.0
    524288u,   // 8.0
    655360u,   // 10.0
};
static_assert(kLadder[sizeof(kLadder) / sizeof(kLadder[0]) - 1] == kFactorMax,
              "the zoom ladder must top out at kFactorMax");
constexpr std::size_t kLadderCount = sizeof(kLadder) / sizeof(kLadder[0]);

// One revision counter for the whole process: RenderSnapshot carries no other
// ordering source and the controller has no counter member to keep.
std::atomic<std::uint64_t> g_revision{0};

// Division that rounds towards negative infinity rather than towards zero, so
// an odd leftover lands on the same side whichever way the operands point. Used
// for centring, where truncation would bias the viewport by a pixel as soon as
// the visible extent passes the selection.
constexpr std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
    const std::int64_t q = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? q - 1 : q;
}

}  // namespace

ViewportMapping compute_viewport(const SelectionConfig& sel,
                                 const MagnificationConfig& mag) noexcept {
    ViewportMapping out;

    const Px src_w = width_of(sel.bounds_px);
    const Px src_h = height_of(sel.bounds_px);
    const Px out_w = mag.output_size_px.width;
    const Px out_h = mag.output_size_px.height;
    if (src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) return out;

    // The factor is the magnification, full stop. The old model scaled the
    // source to *fit* the window, which made the window size and the factor two
    // ways of saying the same thing: drag an edge and the picture zoomed, while
    // the factor box went on claiming the old number -- and on the shipped
    // defaults (a 320x240 region claiming 4x in a 640x480 window) it was wrong
    // from the first frame. Deriving the scale from the factor instead means no
    // resize can contradict the read-out.
    Q16 scale = mag.factor_q16;
    if (scale < kFactorMin) scale = kFactorMin;
    if (scale > kFactorMax) scale = kFactorMax;
    out.applied_scale_q16 = scale;
    out.valid = true;
    out.dest_rect_px = RectPx{0, 0, out_w, out_h};

    // The source extent that fills the client at that scale, centred on the
    // region and free to leave it: the viewport is a window onto the desktop,
    // so a bigger window shows more of the screen rather than a bigger picture.
    const Px vis_w = static_cast<Px>(
        std::max<std::int64_t>(1, (static_cast<std::int64_t>(out_w) << 16) / scale));
    const Px vis_h = static_cast<Px>(
        std::max<std::int64_t>(1, (static_cast<std::int64_t>(out_h) << 16) / scale));
    // Floored rather than truncated, so the region stays centred to the pixel
    // when the visible extent is odd and the window is not an exact multiple.
    const auto centred = [](Px extent, Px visible) noexcept {
        return static_cast<Px>(floor_div(static_cast<std::int64_t>(extent - visible), 2));
    };
    const Px left = centred(src_w, vis_w);
    const Px top = centred(src_h, vis_h);
    out.src_sub_rect_px = RectPx{left, top, left + vis_w, top + vis_h};
    return out;
}

SizePx default_output_size(const SelectionConfig& sel, Q16 factor) noexcept {
    const Px w = width_of(sel.bounds_px);
    const Px h = height_of(sel.bounds_px);
    if (w <= 0 || h <= 0) return SizePx{0, 0};
    return SizePx{scale_px(w, factor), scale_px(h, factor)};
}

Q16 ladder_step(Q16 current, int direction) noexcept {
    if (direction > 0) {
        for (std::size_t i = 0; i < kLadderCount; ++i) {
            if (kLadder[i] > current) return kLadder[i];
        }
        return kLadder[kLadderCount - 1];
    }
    if (direction < 0) {
        for (std::size_t i = kLadderCount; i-- > 0;) {
            if (kLadder[i] < current) return kLadder[i];
        }
        return kLadder[0];
    }
    return current;
}

// ---------------------------------------------------------------------------
// MagnificationController
// ---------------------------------------------------------------------------

MagnificationController::MagnificationController(IEventBus& bus, RectPx virtual_bounds_px)
    : bus_(bus), virtual_bounds_px_(normalize(virtual_bounds_px)) {
    if (is_empty(virtual_bounds_px_)) {
        throw std::invalid_argument("virtual bounds must not be empty");
    }
    // The shipped default window must be reachable on the desktop in hand.
    config_.output_size_px = SizePx{
        clamp_px(config_.output_size_px.width, kMinOutputEdgePx,
                 largest(kMinOutputEdgePx, width_of(virtual_bounds_px_))),
        clamp_px(config_.output_size_px.height, kMinOutputEdgePx,
                 largest(kMinOutputEdgePx, height_of(virtual_bounds_px_)))};
    clamp_position();
}

void MagnificationController::set(const MagnificationConfig& cfg) {
    config_ = cfg;
    // set() documents no exception, so an out-of-range value from a caller is
    // repaired rather than thrown on.
    config_.factor_q16 = clamp_q16(config_.factor_q16, kFactorMin, kFactorMax);
    const Px max_w = largest(kMinOutputEdgePx, width_of(virtual_bounds_px_));
    const Px max_h = largest(kMinOutputEdgePx, height_of(virtual_bounds_px_));
    config_.output_size_px.width =
        clamp_px(config_.output_size_px.width, kMinOutputEdgePx, max_w);
    config_.output_size_px.height =
        clamp_px(config_.output_size_px.height, kMinOutputEdgePx, max_h);
    clamp_position();
    publish_current();
}

void MagnificationController::set_factor(Q16 factor_q16) {
    if (factor_q16 < kFactorMin || factor_q16 > kFactorMax) {
        throw std::out_of_range("magnification factor outside [kFactorMin, kFactorMax]");
    }
    if (factor_q16 == config_.factor_q16) {
        publish_current();
        return;
    }
    config_.factor_q16 = factor_q16;
    publish_current();
}

Q16 MagnificationController::select_preset(std::size_t index) {
    if (index >= kPresetCount) {
        throw std::out_of_range("preset index outside [0, kPresetCount)");
    }
    last_preset_ = index;
    config_.factor_q16 = presets_[index];
    publish_current();
    return config_.factor_q16;
}

void MagnificationController::set_presets(const std::array<Q16, kPresetCount>& presets) noexcept {
    for (std::size_t i = 0; i < kPresetCount; ++i) {
        // The table is indexed by a hotkey, so every slot has to hold something
        // usable even if the configuration left one unset.
        presets_[i] = (presets[i] >= kFactorMin && presets[i] <= kFactorMax) ? presets[i]
                                                                            : presets_[i];
    }
}

void MagnificationController::step_factor(int steps) {
    if (steps == 0) return;
    // More than one full ladder traversal is a no-op, so bounding the count
    // also makes an INT_MIN step safe.
    if (steps > static_cast<int>(kLadderCount)) steps = static_cast<int>(kLadderCount);
    if (steps < -static_cast<int>(kLadderCount)) steps = -static_cast<int>(kLadderCount);

    const int direction = steps > 0 ? 1 : -1;
    Q16 factor = config_.factor_q16;
    const int count = steps > 0 ? steps : -steps;
    for (int i = 0; i < count; ++i) factor = ladder_step(factor, direction);

    if (factor == config_.factor_q16) return;
    config_.factor_q16 = factor;
    publish_current();
}

void MagnificationController::resize_output(SizePx size_px) {
    size_px = with_ratio_lock(size_px);
    const Px max_w = width_of(virtual_bounds_px_);
    const Px max_h = height_of(virtual_bounds_px_);
    if (size_px.width < kMinOutputEdgePx || size_px.height < kMinOutputEdgePx ||
        size_px.width > max_w || size_px.height > max_h) {
        throw std::out_of_range("output size outside [kMinOutputEdgePx, virtual desktop]");
    }
    const PointPx top_left = centred_top_left(output_rect(), size_px);
    config_.output_size_px = size_px;
    output_position_px_ = top_left;
    clamp_position();
    publish_current();
}

// The proportion the window has now, applied to whichever axis the caller did
// not move. The derived side is clamped into the legal range rather than
// throwing: a drag that reaches the edge of the desktop should stop growing, not
// make the whole resize silently do nothing.
SizePx MagnificationController::with_ratio_lock(SizePx size_px) const noexcept {
    if (!config_.keep_aspect_ratio) return size_px;
    const SizePx cur = config_.output_size_px;
    if (cur.width <= 0 || cur.height <= 0) return size_px;
    if (size_px.width <= 0 || size_px.height <= 0) return size_px;

    const bool width_moved = size_px.width != cur.width;
    const bool height_moved = size_px.height != cur.height;
    if (!width_moved && !height_moved) return size_px;

    const Px max_w = largest(kMinOutputEdgePx, width_of(virtual_bounds_px_));
    const Px max_h = largest(kMinOutputEdgePx, height_of(virtual_bounds_px_));
    // Rounded rather than truncated, so a width and the height derived from it
    // come back to the size they started as when the caller undoes the change.
    const auto derived = [](Px known, Px known_ref, Px other_ref) -> Px {
        const std::int64_t v =
            (static_cast<std::int64_t>(known) * other_ref + known_ref / 2) / known_ref;
        return static_cast<Px>(v < 1 ? 1 : v);
    };

    if (width_moved) {
        const Px wanted = derived(size_px.width, cur.width, cur.height);
        const Px height = clamp_px(wanted, kMinOutputEdgePx, max_h);
        if (height != wanted) {
            // The derived side hit the desktop, so the moved side comes back
            // with it -- but only while it is a legal size itself. A width that
            // is out of range has to reach resize_output()'s range check
            // unchanged, so that it is still refused rather than quietly turned
            // into something else.
            size_px.height = height;
            if (size_px.width >= kMinOutputEdgePx && size_px.width <= max_w) {
                size_px.width = derived(height, cur.height, cur.width);
            }
            return size_px;
        }
        size_px.height = height;
        return size_px;
    }

    const Px wanted = derived(size_px.height, cur.height, cur.width);
    const Px width = clamp_px(wanted, kMinOutputEdgePx, max_w);
    if (width != wanted) {
        size_px.width = width;
        if (size_px.height >= kMinOutputEdgePx && size_px.height <= max_h) {
            size_px.height = derived(width, cur.width, cur.height);
        }
        return size_px;
    }
    size_px.width = width;
    return size_px;
}

void MagnificationController::step_output_size(Px dx, Px dy) {
    const Px max_w = largest(kMinOutputEdgePx, width_of(virtual_bounds_px_));
    const Px max_h = largest(kMinOutputEdgePx, height_of(virtual_bounds_px_));
    const Px w = clamp_px(add_sat(config_.output_size_px.width, dx), kMinOutputEdgePx, max_w);
    const Px h = clamp_px(add_sat(config_.output_size_px.height, dy), kMinOutputEdgePx, max_h);
    if (w == config_.output_size_px.width && h == config_.output_size_px.height) return;
    const SizePx size = with_ratio_lock(SizePx{w, h});
    const PointPx top_left = centred_top_left(output_rect(), size);
    config_.output_size_px = size;
    output_position_px_ = top_left;
    clamp_position();
    publish_current();
}

void MagnificationController::set_keep_aspect_ratio(bool keep) {
    if (keep == config_.keep_aspect_ratio) return;
    config_.keep_aspect_ratio = keep;
    publish_current();
}

void MagnificationController::set_output_position(PointPx top_left_px) {
    output_position_px_ = top_left_px;
    clamp_position();
    publish_current();
}

void MagnificationController::center_on(RectPx monitor_rect_px) {
    const RectPx monitor = normalize(monitor_rect_px);
    const SizePx size = config_.output_size_px;
    output_position_px_ =
        PointPx{add_sat(monitor.left, (width_of(monitor) - size.width) / 2),
                add_sat(monitor.top, (height_of(monitor) - size.height) / 2)};
    clamp_position();
    publish_current();
}

void MagnificationController::set_virtual_bounds(RectPx virtual_bounds_px) {
    const RectPx bounds = normalize(virtual_bounds_px);
    // The constructor rejects empty bounds, but a later topology report may be
    // degenerate; keeping the previous bounds beats parking the window at 0,0.
    if (is_empty(bounds)) return;
    virtual_bounds_px_ = bounds;
    clamp_position();
    publish_current();
}

void MagnificationController::fit_output_to_selection(const SelectionConfig& sel) {
    const SizePx fitted = default_output_size(sel, config_.factor_q16);
    const Px max_w = largest(kMinOutputEdgePx, width_of(virtual_bounds_px_));
    const Px max_h = largest(kMinOutputEdgePx, height_of(virtual_bounds_px_));
    const SizePx size{clamp_px(fitted.width, kMinOutputEdgePx, max_w),
                      clamp_px(fitted.height, kMinOutputEdgePx, max_h)};
    // Zooming grows the window around its middle like any other resize, rather
    // than from its top-left corner.
    const PointPx top_left = centred_top_left(output_rect(), size);
    config_.output_size_px = size;
    output_position_px_ = top_left;
    clamp_position();
    publish_current();
}

RectPx MagnificationController::output_rect() const noexcept {
    const SizePx size = config_.output_size_px;
    const RectPx raw{output_position_px_.x, output_position_px_.y,
                     add_sat(output_position_px_.x, size.width),
                     add_sat(output_position_px_.y, size.height)};
    return clamp_into(raw, virtual_bounds_px_);
}

void MagnificationController::republish() { publish_current(); }

void MagnificationController::publish_current() {
    RenderSnapshot snapshot;
    snapshot.magnification = config_;
    snapshot.revision = ++g_revision;
    bus_.publish(AppEvent::make(SnapshotChanged{snapshot}));
}

void MagnificationController::clamp_position() {
    // output_rect() already applies the desktop clamp, so re-reading it leaves
    // the stored position consistent with what the window manager will show.
    const RectPx clamped = output_rect();
    output_position_px_ = PointPx{clamped.left, clamped.top};
}

}  // namespace mag
