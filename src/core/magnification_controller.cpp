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

// Q16.16 ratio dst/src, floored. Flooring guarantees a contain fit never
// overshoots the client area once the source edge is scaled back up, and the
// saturating cast keeps an absurd output size from wrapping into a tiny scale.
constexpr Q16 ratio_q16(Px dst, Px src) noexcept {
    const std::int64_t ratio = (static_cast<std::int64_t>(dst) << 16) / src;
    if (ratio > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
    return static_cast<Q16>(ratio);
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

    const Q16 ratio_x = ratio_q16(out_w, src_w);
    const Q16 ratio_y = ratio_q16(out_h, src_h);
    // Both modes stay uniform: only the smaller (contain) or the larger
    // (cover) of the two axis ratios is ever applied. A source larger than
    // 65536 px per axis floors both ratios to zero, so the scale is nudged to
    // the smallest representable step before it can be divided by.
    Q16 scale = mag.keep_aspect_ratio ? (ratio_x < ratio_y ? ratio_x : ratio_y)
                                      : (ratio_x > ratio_y ? ratio_x : ratio_y);
    if (scale == 0) scale = 1;
    out.applied_scale_q16 = scale;
    out.valid = true;

    if (mag.keep_aspect_ratio) {
        // Contain: the whole source remains visible and the leftover area is
        // letterboxed, so src_sub_rect is simply the full extent.
        const Px dest_w = scale_px(src_w, scale);
        const Px dest_h = scale_px(src_h, scale);
        const Px left = (out_w - dest_w) / 2;
        const Px top = (out_h - dest_h) / 2;
        out.dest_rect_px = RectPx{left, top, left + dest_w, top + dest_h};
        out.src_sub_rect_px = RectPx{0, 0, src_w, src_h};
        out.letterboxed = dest_w != out_w || dest_h != out_h;
        return out;
    }

    // Cover: the client area is filled, and this is the only path allowed to
    // crop. Invert the scale to find the source window that actually shows,
    // then centre it; the clamp keeps rounding from admitting source pixels
    // that no longer exist.
    out.dest_rect_px = RectPx{0, 0, out_w, out_h};
    Px vis_w = static_cast<Px>((static_cast<std::int64_t>(out_w) << 16) / scale);
    Px vis_h = static_cast<Px>((static_cast<std::int64_t>(out_h) << 16) / scale);
    vis_w = clamp_px(vis_w, 1, src_w);
    vis_h = clamp_px(vis_h, 1, src_h);
    const Px src_left = (src_w - vis_w) / 2;
    const Px src_top = (src_h - vis_h) / 2;
    out.src_sub_rect_px =
        RectPx{src_left, src_top, src_left + vis_w, src_top + vis_h};
    out.letterboxed = false;
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

void MagnificationController::step_output_size(Px dx, Px dy) {
    const Px max_w = largest(kMinOutputEdgePx, width_of(virtual_bounds_px_));
    const Px max_h = largest(kMinOutputEdgePx, height_of(virtual_bounds_px_));
    const Px w = clamp_px(add_sat(config_.output_size_px.width, dx), kMinOutputEdgePx, max_w);
    const Px h = clamp_px(add_sat(config_.output_size_px.height, dy), kMinOutputEdgePx, max_h);
    if (w == config_.output_size_px.width && h == config_.output_size_px.height) return;
    const SizePx size{w, h};
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
