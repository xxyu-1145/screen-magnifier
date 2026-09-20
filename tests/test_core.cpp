// tests/test_core.cpp — self-contained unit tests for the core domain layer.
//
// No test framework: the acceptance criteria are about exact integer geometry,
// so a tiny check macro plus a fixed seed keeps this buildable with the same
// zig/clang invocation as the app.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

#include "core/config.h"
#include "core/event_bus.h"
#include "core/hit_test.h"
#include "core/interaction_state_machine.h"
#include "core/magnification_controller.h"
#include "core/selection_controller.h"
#include "core/types.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

#define CHECK_THROWS(expr, extype)                                                        \
    do {                                                                                  \
        bool threw = false;                                                               \
        try {                                                                             \
            (void)(expr);                                                                 \
        } catch (const extype&) {                                                         \
            threw = true;                                                                 \
        } catch (...) {                                                                   \
        }                                                                                 \
        ++g_checks;                                                                       \
        if (!threw) {                                                                     \
            ++g_failures;                                                                 \
            std::printf("FAIL line %d: %s did not throw %s\n", __LINE__, #expr, #extype); \
        }                                                                                 \
    } while (false)

using mag::Px;
using mag::Q16;
using mag::RectPx;

RectPx mk(Px l, Px t, Px r, Px b) { return RectPx{l, t, r, b}; }

bool same_rect(const RectPx& r, Px l, Px t, Px rr, Px b) {
    return r.left == l && r.top == t && r.right == rr && r.bottom == b;
}

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

class RecordingBus final : public mag::IEventBus {
public:
    bool publish(const mag::AppEvent& event) noexcept override {
        events.push_back(event.payload);
        return true;
    }
    std::size_t drain(std::size_t) override { return 0; }

    std::vector<mag::Payload> events;
};

template <typename T>
std::size_t count_of(const std::vector<mag::Payload>& events) {
    std::size_t n = 0;
    for (const mag::Payload& p : events) {
        if (std::holds_alternative<T>(p)) ++n;
    }
    return n;
}

bool last_selection(const RecordingBus& bus, mag::SelectionConfig& out) {
    for (auto it = bus.events.rbegin(); it != bus.events.rend(); ++it) {
        if (const auto* sc = std::get_if<mag::SelectionChanged>(&*it)) {
            out = sc->value;
            return true;
        }
    }
    return false;
}

struct HandlerProbe {
    int count{0};
    int first_index{-1};
    int last_index{-1};
    std::uint32_t last_id{0};
};

void count_handler(void* ctx, const mag::AppEvent& event) {
    auto* probe = static_cast<HandlerProbe*>(ctx);
    if (probe->count == 0) probe->first_index = static_cast<int>(event.payload.index());
    probe->last_index = static_cast<int>(event.payload.index());
    if (const auto* hk = std::get_if<mag::HotkeyPressed>(&event.payload)) probe->last_id = hk->id;
    ++probe->count;
}

class FakeOverlay final : public mag::IOverlayWindow {
public:
    void show() noexcept override { shown = true; }
    void hide() noexcept override { shown = false; }
    void set_hit_test_transparent(bool transparent) override { hit_transparent = transparent; }
    void show_edge_hint() noexcept override { hint = true; }
    void hide_edge_hint() noexcept override { hint = false; }

    bool shown{false};
    bool hit_transparent{false};
    bool hint{false};
};

class FakeCapture final : public mag::ICaptureControl {
public:
    void start() noexcept override { ++starts; }
    void stop() noexcept override { ++stops; }
    void recover_async() noexcept override { ++recovers; }

    int starts{0};
    int stops{0};
    int recovers{0};
};

// ---------------------------------------------------------------------------
// §3.1 shape membership
// ---------------------------------------------------------------------------

void test_shapes() {
    const mag::SelectionConfig rect{mk(0, 0, 10, 10), mag::SelectionShape::Rectangle, 0};
    CHECK(mag::point_in_shape(rect, {0, 0}));
    CHECK(mag::point_in_shape(rect, {9, 9}));
    CHECK(!mag::point_in_shape(rect, {10, 10}));  // half-open
    CHECK(!mag::point_in_shape(rect, {-1, 0}));
    CHECK(!mag::point_in_shape(rect, {5, 10}));

    // Circle: squared to min(w,h) and centred, so a wide rect hits only the
    // middle band.
    const mag::SelectionConfig circle{mk(0, 0, 20, 10), mag::SelectionShape::Circle, 0};
    CHECK(same_rect(mag::shape_bounds(circle), 5, 0, 15, 10));
    CHECK(mag::point_in_shape(circle, {10, 5}));
    CHECK(mag::point_in_shape(circle, {5, 5}));
    CHECK(!mag::point_in_shape(circle, {0, 5}));   // outside the squared box
    CHECK(!mag::point_in_shape(circle, {19, 5}));
    CHECK(mag::point_in_shape(circle, {10, 0}));   // on the boundary at the top
    CHECK(!mag::point_in_shape(circle, {15, 0}));  // corner of the squared box
    CHECK(!mag::point_in_shape(circle, {5, 0}));

    const mag::SelectionConfig square_circle{mk(0, 0, 10, 10), mag::SelectionShape::Circle, 0};
    CHECK(same_rect(mag::shape_bounds(square_circle), 0, 0, 10, 10));
    CHECK(mag::point_in_shape(square_circle, {5, 5}));
    CHECK(mag::point_in_shape(square_circle, {0, 5}));
    CHECK(!mag::point_in_shape(square_circle, {0, 0}));

    // Ellipse: independent half-axes.
    const mag::SelectionConfig ellipse{mk(0, 0, 20, 10), mag::SelectionShape::Ellipse, 0};
    CHECK(mag::point_in_shape(ellipse, {10, 5}));
    CHECK(mag::point_in_shape(ellipse, {0, 5}));
    CHECK(mag::point_in_shape(ellipse, {19, 5}));
    CHECK(mag::point_in_shape(ellipse, {10, 0}));
    CHECK(!mag::point_in_shape(ellipse, {0, 0}));
    CHECK(!mag::point_in_shape(ellipse, {19, 9}));

    // Rounded rectangle: centre bands plus the four corner quadrants.
    const mag::SelectionConfig rounded{mk(0, 0, 20, 20), mag::SelectionShape::RoundedRectangle, 5};
    CHECK(mag::point_in_shape(rounded, {0, 10}));
    CHECK(mag::point_in_shape(rounded, {10, 0}));
    CHECK(mag::point_in_shape(rounded, {5, 5}));
    CHECK(mag::point_in_shape(rounded, {2, 2}));
    CHECK(!mag::point_in_shape(rounded, {1, 1}));
    CHECK(!mag::point_in_shape(rounded, {0, 0}));
    CHECK(!mag::point_in_shape(rounded, {19, 0}));
    CHECK(!mag::point_in_shape(rounded, {0, 19}));
    CHECK(!mag::point_in_shape(rounded, {25, 10}));

    // A zero radius degenerates to the plain rectangle.
    const mag::SelectionConfig sharp{mk(0, 0, 20, 20), mag::SelectionShape::RoundedRectangle, 0};
    CHECK(mag::point_in_shape(sharp, {0, 0}));
    CHECK(mag::point_in_shape(sharp, {19, 19}));
    CHECK(!mag::point_in_shape(sharp, {20, 19}));

    // Empty geometry never claims a pixel.
    const mag::SelectionConfig empty{mk(5, 5, 5, 20), mag::SelectionShape::Rectangle, 0};
    CHECK(!mag::point_in_shape(empty, {5, 5}));

    // Far larger than any real virtual desktop: every intermediate product
    // still fits in int64, so the result stays exact.
    const mag::SelectionConfig huge{mk(-40000, -40000, 40000, 40000),
                                    mag::SelectionShape::Ellipse, 0};
    CHECK(mag::point_in_shape(huge, {0, 0}));
    CHECK(mag::point_in_shape(huge, {0, 39999}));
    CHECK(!mag::point_in_shape(huge, {39999, 39999}));
}

void test_handles() {
    const mag::SelectionConfig sel{mk(0, 0, 100, 100), mag::SelectionShape::Rectangle, 0};
    CHECK(mag::handle_at(sel, {0, 0}) == mag::ResizeHandle::TopLeft);
    CHECK(mag::handle_at(sel, {99, 99}) == mag::ResizeHandle::BottomRight);
    CHECK(mag::handle_at(sel, {99, 0}) == mag::ResizeHandle::TopRight);
    CHECK(mag::handle_at(sel, {0, 99}) == mag::ResizeHandle::BottomLeft);
    CHECK(mag::handle_at(sel, {0, 50}) == mag::ResizeHandle::Left);
    CHECK(mag::handle_at(sel, {99, 50}) == mag::ResizeHandle::Right);
    CHECK(mag::handle_at(sel, {50, 0}) == mag::ResizeHandle::Top);
    CHECK(mag::handle_at(sel, {50, 99}) == mag::ResizeHandle::Bottom);
    CHECK(mag::handle_at(sel, {50, 50}) == mag::ResizeHandle::Move);
    CHECK(mag::handle_at(sel, {4, 50}) == mag::ResizeHandle::Left);
    CHECK(mag::handle_at(sel, {5, 50}) == mag::ResizeHandle::Move);
    CHECK(mag::handle_at(sel, {-1, 50}) == mag::ResizeHandle::None);
    CHECK(mag::handle_at(sel, {150, 50}) == mag::ResizeHandle::None);
    CHECK(mag::handle_at(sel, {50, 500}) == mag::ResizeHandle::None);
    CHECK(mag::handle_at(sel, {50, 50}, 0) == mag::ResizeHandle::Move);

    // A corner of the bounding rect is outside the circle.
    const mag::SelectionConfig circle{mk(0, 0, 100, 100), mag::SelectionShape::Circle, 0};
    CHECK(mag::handle_at(circle, {0, 0}) == mag::ResizeHandle::None);
    CHECK(mag::handle_at(circle, {3, 50}) == mag::ResizeHandle::Left);
}

void test_corner_radius_and_conform() {
    CHECK(mag::clamp_corner_radius(100, mk(0, 0, 20, 10)) == 5);
    CHECK(mag::clamp_corner_radius(3, mk(0, 0, 20, 10)) == 3);
    CHECK(mag::clamp_corner_radius(0, mk(0, 0, 20, 10)) == 0);
    CHECK(mag::clamp_corner_radius(-4, mk(0, 0, 20, 10)) == 0);
    CHECK(mag::clamp_corner_radius(7, mk(0, 0, 0, 0)) == 0);

    const mag::SelectionConfig circle{mk(0, 0, 20, 10), mag::SelectionShape::Circle, 9};
    const mag::SelectionConfig conformed = mag::conform_to_shape(circle);
    CHECK(same_rect(conformed.bounds_px, 5, 0, 15, 10));
    CHECK(conformed.corner_radius_px == 5);

    const mag::SelectionConfig rounded{mk(0, 0, 20, 10), mag::SelectionShape::RoundedRectangle, 99};
    const mag::SelectionConfig rounded_ok = mag::conform_to_shape(rounded);
    CHECK(same_rect(rounded_ok.bounds_px, 0, 0, 20, 10));
    CHECK(rounded_ok.corner_radius_px == 5);

    const mag::SelectionConfig rect{mk(0, 0, 20, 10), mag::SelectionShape::Rectangle, 7};
    const mag::SelectionConfig rect_ok = mag::conform_to_shape(rect);
    // A rectangle ignores the corner radius but must not discard it: zeroing it
    // here meant switching to Rounded and back silently produced a plain
    // rectangle, because the radius the user chose had been thrown away.
    CHECK(rect_ok.corner_radius_px == 5);
    CHECK(same_rect(rect_ok.bounds_px, 0, 0, 20, 10));
}

// ---------------------------------------------------------------------------
// §5.4 fixed-point exactness
// ---------------------------------------------------------------------------

void test_fixed_point() {
    const Q16 factors[] = {mag::kQ16One, mag::kQ16One * 2, mag::kQ16One * 4, mag::kQ16One * 8,
                           mag::kFactorMax};
    for (Q16 f : factors) {
        CHECK(mag::scale_px(0, f) == 0);
        CHECK(mag::scale_px(1, f) == static_cast<Px>(f / mag::kQ16One));
        CHECK(mag::scale_px(320, f) == static_cast<Px>(320 * (f / mag::kQ16One)));
    }
    CHECK(mag::scale_px(320, mag::kQ16One) == 320);
    CHECK(mag::scale_px(320, mag::kQ16One * 2) == 640);
    CHECK(mag::scale_px(320, mag::kQ16One * 4) == 1280);
    CHECK(mag::scale_px(320, mag::kQ16One * 8) == 2560);
    CHECK(mag::scale_px(320, mag::kFactorMax) == 3200);
    CHECK(mag::scale_px(120, mag::kFactorMax) == 1200);
    CHECK(mag::scale_px(1, mag::kQ16One + mag::kQ16One / 2) == 2);  // half rounds up
    CHECK(mag::scale_px(3, mag::kQ16One / 2) == 2);                 // 1.5 -> 2
    CHECK(mag::scale_px(16384, mag::kFactorMax) == 163840);         // 10x, no overflow
    CHECK(mag::q16_to_double(mag::kQ16One * 4) == 4.0);
    CHECK(mag::q16_from_double(2.5) == 163840u);
}

// ---------------------------------------------------------------------------
// Event bus and snapshot mailbox
// ---------------------------------------------------------------------------

void test_event_bus() {
    {
        mag::BoundedEventBus bus(2);
        CHECK(bus.size() == 0);
        CHECK(bus.dropped_count() == 0);
        CHECK(bus.publish(mag::make_event(mag::StateChanged{mag::InteractionState::Off})));
        CHECK(bus.publish(mag::make_event(mag::StateChanged{mag::InteractionState::Interactive})));
        CHECK(bus.size() == 2);
        CHECK(!bus.publish(mag::make_event(mag::StateChanged{mag::InteractionState::Off})));
        CHECK(bus.dropped_count() == 1);
        CHECK(bus.size() == 2);
        CHECK(bus.drain(1) == 1);
        CHECK(bus.size() == 1);
        CHECK(bus.drain(5) == 1);
        CHECK(bus.drain(5) == 0);
        CHECK_THROWS(bus.drain(0), std::invalid_argument);
    }
    {
        const int hotkey_index = static_cast<int>(mag::Payload{mag::HotkeyPressed{}}.index());
        const int quit_index = static_cast<int>(mag::Payload{mag::QuitRequested{}}.index());

        mag::BoundedEventBus bus(8);
        HandlerProbe probe;
        bus.set_handler(&count_handler, &probe);
        CHECK(bus.publish(mag::make_event(mag::HotkeyPressed{3})));
        CHECK(bus.publish(mag::make_event(mag::QuitRequested{})));
        CHECK(bus.publish(mag::make_event(mag::HotkeyPressed{4})));
        CHECK(bus.drain(2) == 2);
        CHECK(probe.count == 2);
        CHECK(probe.first_index == hotkey_index);
        CHECK(probe.last_index == quit_index);
        CHECK(bus.size() == 1);
        CHECK(bus.drain(10) == 1);
        CHECK(probe.count == 3);
        CHECK(probe.last_index == hotkey_index);
        CHECK(probe.last_id == 4u);
    }
    {
        // Timestamp and source thread survive the round trip.
        const mag::AppEvent meta = mag::make_event(mag::QuitRequested{}, 12345, 7);
        CHECK(meta.timestamp_qpc == 12345u);
        CHECK(meta.source_thread == 7u);
        CHECK(std::holds_alternative<mag::QuitRequested>(meta.payload));
    }
    {
        mag::SnapshotMailbox box;
        mag::SnapshotMailbox::Entry entry;
        mag::RenderSnapshot snap;
        std::uint64_t last = 0;
        CHECK(!box.take_latest(entry, last));
        CHECK(!box.peek(snap));
        CHECK(box.sequence() == 0);

        mag::RenderSnapshot s;
        s.revision = 42;
        s.magnification.factor_q16 = mag::kQ16One * 2;
        box.submit(s);
        CHECK(box.sequence() == 1);
        CHECK(box.peek(snap));
        CHECK(snap == s);
        CHECK(box.take_latest(entry, last));
        CHECK(entry.sequence == 1);
        CHECK(last == 1);
        CHECK(entry.snapshot == s);
        CHECK(!box.take_latest(entry, last));  // nothing newer
        box.submit(s);
        CHECK(box.sequence() == 2);
        CHECK(box.take_latest(entry, last));
        CHECK(entry.sequence == 2);
        CHECK(last == 2);
    }
    {
        mag::HotkeyAction out{};
        CHECK(std::strcmp(mag::hotkey_action_name(mag::HotkeyAction::ToggleMagnifier),
                          "ToggleMagnifier") == 0);
        CHECK(std::strcmp(mag::hotkey_action_name(mag::HotkeyAction::TogglePassThrough),
                          "TogglePassThrough") == 0);
        CHECK(std::strcmp(mag::hotkey_action_name(mag::HotkeyAction::Preset3), "Preset3") == 0);
        CHECK(std::strcmp(mag::hotkey_action_name(mag::HotkeyAction::ShrinkHeight),
                          "ShrinkHeight") == 0);
        CHECK(std::strcmp(mag::hotkey_action_name(mag::HotkeyAction::Quit), "Quit") == 0);
        for (std::size_t i = 0; i < mag::kHotkeyCount; ++i) {
            const auto action = static_cast<mag::HotkeyAction>(i);
            const char* name = mag::hotkey_action_name(action);
            CHECK(name != nullptr);
            CHECK(mag::hotkey_action_from_name(name, out));
            CHECK(out == action);
            CHECK(static_cast<std::size_t>(out) == i);
        }
        CHECK(!mag::hotkey_action_from_name(nullptr, out));
        CHECK(!mag::hotkey_action_from_name("", out));
        CHECK(!mag::hotkey_action_from_name("toggleMagnifier", out));
        CHECK(!mag::hotkey_action_from_name("Bogus", out));
    }
}

// ---------------------------------------------------------------------------
// §5.3 selection controller
// ---------------------------------------------------------------------------

void test_selection_controller() {
    const RectPx vb = mk(0, 0, 800, 600);

    RecordingBus bus;
    CHECK_THROWS(mag::SelectionController(bus, mk(5, 5, 5, 60)), std::invalid_argument);
    CHECK_THROWS(mag::SelectionController(bus, mk(0, 0, 0, 0)), std::invalid_argument);

    mag::SelectionController c(bus, vb);
    CHECK(c.virtual_bounds() == vb);
    CHECK(!c.dragging());

    c.set({mk(100, 100, 300, 200), mag::SelectionShape::Rectangle, 0});
    CHECK(same_rect(c.current().bounds_px, 100, 100, 300, 200));
    CHECK(count_of<mag::SelectionChanged>(bus.events) == 1);
    CHECK_THROWS(c.set({mk(10, 10, 10, 40), mag::SelectionShape::Rectangle, 0}),
                 std::invalid_argument);
    CHECK(same_rect(c.current().bounds_px, 100, 100, 300, 200));

    // A config rect larger than the desktop is clipped into it.
    c.set({mk(-50, -50, 5000, 5000), mag::SelectionShape::Rectangle, 0});
    CHECK(same_rect(c.current().bounds_px, 0, 0, 800, 600));

    // transform touches only the edges the handle names.
    c.set({mk(100, 100, 300, 200), mag::SelectionShape::Rectangle, 0});
    c.transform(mag::ResizeHandle::Left, {10, 0});
    CHECK(same_rect(c.current().bounds_px, 110, 100, 300, 200));
    c.transform(mag::ResizeHandle::Left, {-10, 0});
    CHECK(same_rect(c.current().bounds_px, 100, 100, 300, 200));
    c.transform(mag::ResizeHandle::BottomRight, {10, 20});
    CHECK(same_rect(c.current().bounds_px, 100, 100, 310, 220));
    c.transform(mag::ResizeHandle::Move, {-10, -20});
    CHECK(same_rect(c.current().bounds_px, 90, 80, 300, 200));
    CHECK_THROWS(c.transform(mag::ResizeHandle::Left, {210, 0}), std::invalid_argument);
    CHECK(same_rect(c.current().bounds_px, 90, 80, 300, 200));
    // Overshooting an edge flips the rect and normalizes it, per §5.3.
    c.transform(mag::ResizeHandle::Left, {250, 0});
    CHECK(same_rect(c.current().bounds_px, 300, 80, 340, 200));

    // A resize stops at the desktop edge instead of dragging the far edge in.
    mag::SelectionController edge(bus, mk(0, 0, 500, 500));
    edge.set({mk(100, 100, 200, 200), mag::SelectionShape::Rectangle, 0});
    edge.transform(mag::ResizeHandle::Right, {1000, 0});
    CHECK(same_rect(edge.current().bounds_px, 100, 100, 500, 200));
    edge.transform(mag::ResizeHandle::Bottom, {0, 1000});
    CHECK(same_rect(edge.current().bounds_px, 100, 100, 500, 500));
    // A move keeps its size and slides back inside the desktop.
    edge.transform(mag::ResizeHandle::Move, {1000, 1000});
    CHECK(same_rect(edge.current().bounds_px, 100, 100, 500, 500));
    edge.move_to({2000, 2000});
    CHECK(same_rect(edge.current().bounds_px, 100, 100, 500, 500));
    edge.move_to({0, 0});
    CHECK(same_rect(edge.current().bounds_px, 0, 0, 400, 400));

    // nudge moves whole pixels and refuses to leave the desktop.
    mag::SelectionController n(bus, mk(0, 0, 500, 600));
    n.set({mk(100, 100, 200, 200), mag::SelectionShape::Rectangle, 0});
    n.nudge(1, 0);
    CHECK(same_rect(n.current().bounds_px, 101, 100, 201, 200));
    n.nudge(-1, 0);
    n.nudge(0, 0);
    CHECK(same_rect(n.current().bounds_px, 100, 100, 200, 200));
    n.set({mk(0, 0, 50, 50), mag::SelectionShape::Rectangle, 0});
    CHECK_THROWS(n.nudge(-1, 0), std::out_of_range);
    CHECK_THROWS(n.nudge(0, -1), std::out_of_range);
    CHECK(same_rect(n.current().bounds_px, 0, 0, 50, 50));
    n.set({mk(450, 550, 500, 600), mag::SelectionShape::Rectangle, 0});
    CHECK_THROWS(n.nudge(1, 0), std::out_of_range);
    CHECK_THROWS(n.nudge(0, 1), std::out_of_range);
    CHECK(same_rect(n.current().bounds_px, 450, 550, 500, 600));

    // grow/shrink every edge, clipped to the desktop.
    mag::SelectionController g(bus, mk(0, 0, 500, 500));
    g.set({mk(100, 100, 200, 200), mag::SelectionShape::Rectangle, 0});
    g.grow(10, 5);
    CHECK(same_rect(g.current().bounds_px, 90, 95, 210, 205));
    g.grow(-10, -5);
    CHECK(same_rect(g.current().bounds_px, 100, 100, 200, 200));
    CHECK_THROWS(g.grow(-1000, 0), std::invalid_argument);
    CHECK_THROWS(g.grow(0, -1000), std::invalid_argument);
    CHECK(same_rect(g.current().bounds_px, 100, 100, 200, 200));
    CHECK_THROWS(g.grow(-100, -100), std::invalid_argument);  // exactly collapses

    // Shapes: a circle equalises its axes, a rectangle clears the radius.
    mag::SelectionController s(bus, mk(0, 0, 1000, 1000));
    s.set({mk(0, 0, 100, 50), mag::SelectionShape::Rectangle, 0});
    s.set_shape(mag::SelectionShape::Circle);
    CHECK(s.current().shape == mag::SelectionShape::Circle);
    CHECK(same_rect(s.current().bounds_px, 25, 0, 75, 50));
    s.set_shape(mag::SelectionShape::RoundedRectangle);
    CHECK(same_rect(s.current().bounds_px, 25, 0, 75, 50));
    s.set_corner_radius(9999);
    CHECK(s.current().corner_radius_px == 25);
    s.set_corner_radius(-5);
    CHECK(s.current().corner_radius_px == 0);
    s.set_corner_radius(10);
    CHECK(s.current().corner_radius_px == 10);
    s.set_shape(mag::SelectionShape::Rectangle);
    // The radius survives a trip through Rectangle, so Round -> Rect -> Round
    // keeps the shape the user configured instead of flattening it.
    CHECK(s.current().corner_radius_px == 10);
    s.set_shape(mag::SelectionShape::RoundedRectangle);
    CHECK(s.current().corner_radius_px == 10);

    // Monitor topology change: keep the size, slide back into view.
    s.set({mk(25, 0, 75, 50), mag::SelectionShape::Rectangle, 0});
    s.set_virtual_bounds(mk(0, 0, 60, 60));
    CHECK(s.virtual_bounds() == mk(0, 0, 60, 60));
    CHECK(same_rect(s.current().bounds_px, 10, 0, 60, 50));
    CHECK_THROWS(s.set_virtual_bounds(mk(0, 0, 0, 0)), std::invalid_argument);
    CHECK(s.virtual_bounds() == mk(0, 0, 60, 60));

    // republish re-emits the current value without changing it.
    const std::size_t before = count_of<mag::SelectionChanged>(bus.events);
    s.republish();
    CHECK(count_of<mag::SelectionChanged>(bus.events) == before + 1);
    mag::SelectionConfig published;
    CHECK(last_selection(bus, published));
    CHECK(published == s.current());
}

void test_selection_drag() {
    RecordingBus bus;
    mag::SelectionController c(bus, mk(0, 0, 800, 600));
    c.set({mk(50, 50, 150, 150), mag::SelectionShape::Rectangle, 0});

    CHECK_THROWS(c.update({1, 1}), std::logic_error);
    CHECK(c.commit() == c.current());  // commit with no drag is a no-op

    c.begin({200, 200});
    CHECK(c.dragging());
    CHECK_THROWS(c.begin({10, 10}), std::logic_error);
    c.update({260, 240});
    CHECK(same_rect(c.current().bounds_px, 200, 200, 260, 240));
    const mag::SizePx size = mag::size_of(c.current().bounds_px);
    CHECK(size.width == 60);
    CHECK(size.height == 40);
    const mag::SelectionConfig committed = c.commit();
    CHECK(!c.dragging());
    CHECK(committed == c.current());
    CHECK(same_rect(committed.bounds_px, 200, 200, 260, 240));
    CHECK_THROWS(c.update({1, 1}), std::logic_error);

    // Dragging in any direction normalizes the rect.
    c.begin({400, 400});
    c.update({350, 300});
    CHECK(same_rect(c.current().bounds_px, 350, 300, 400, 400));
    c.commit();

    // A click without movement keeps the previous selection.
    c.begin({500, 500});
    c.update({500, 500});
    CHECK(same_rect(c.current().bounds_px, 350, 300, 400, 400));
    const mag::SelectionConfig click = c.commit();
    CHECK(same_rect(click.bounds_px, 350, 300, 400, 400));
    CHECK(click == c.current());

    // A drag runs into the desktop edge and is clipped there.
    c.begin({10, 10});
    c.update({5000, 5000});
    CHECK(same_rect(c.current().bounds_px, 10, 10, 800, 600));
    CHECK(c.commit().bounds_px == mk(10, 10, 800, 600));

    // A drag that never reaches a pixel keeps the selection it started from.
    c.begin({600, 500});
    CHECK(c.commit().bounds_px == mk(10, 10, 800, 600));
}

void test_random_round_trips() {
    std::mt19937 rng(20260918u);
    RecordingBus bus;
    mag::SelectionController c(bus, mk(-100, -50, 1820, 1030));

    const mag::ResizeHandle handles[] = {
        mag::ResizeHandle::Move,     mag::ResizeHandle::Left,       mag::ResizeHandle::Right,
        mag::ResizeHandle::Top,      mag::ResizeHandle::Bottom,     mag::ResizeHandle::TopLeft,
        mag::ResizeHandle::TopRight, mag::ResizeHandle::BottomLeft, mag::ResizeHandle::BottomRight};

    for (int i = 0; i < 100; ++i) {
        const Px w = 20 + static_cast<Px>(rng() % 300);
        const Px h = 20 + static_cast<Px>(rng() % 300);
        const Px l = 100 + static_cast<Px>(rng() % 400);
        const Px t = 100 + static_cast<Px>(rng() % 300);
        // Radius 2 survives every +/-15 edge move below, so the geometry can be
        // compared exactly instead of being masked by a radius re-clamp.
        c.set({mk(l, t, l + w, t + h), mag::SelectionShape::RoundedRectangle, 2});
        const RectPx start = c.current().bounds_px;
        CHECK(c.current().corner_radius_px == 2);

        const Px dx = static_cast<Px>(rng() % 31) - 15;
        const Px dy = static_cast<Px>(rng() % 31) - 15;

        for (mag::ResizeHandle handle : handles) {
            c.transform(handle, {dx, dy});
            c.transform(handle, {-dx, -dy});
            CHECK(c.current().bounds_px == start);  // zero physical-pixel drift
            CHECK(c.current().corner_radius_px == 2);
        }

        const Px nx = static_cast<Px>(rng() % 3) - 1;
        const Px ny = static_cast<Px>(rng() % 3) - 1;
        c.nudge(nx, ny);
        CHECK(same_rect(c.current().bounds_px, start.left + nx, start.top + ny, start.right + nx,
                        start.bottom + ny));
        c.nudge(-nx, -ny);
        CHECK(c.current().bounds_px == start);

        c.grow(5, 3);
        c.grow(-5, -3);
        CHECK(c.current().bounds_px == start);
        CHECK(c.current().corner_radius_px == 2);

        // Resizing never lets an edge cross the opposite one.
        c.transform(mag::ResizeHandle::Left, {w - 1, 0});
        CHECK(same_rect(c.current().bounds_px, start.right - 1, start.top, start.right,
                        start.bottom));
        c.transform(mag::ResizeHandle::Left, {-(w - 1), 0});
        CHECK(c.current().bounds_px == start);
    }
}

// ---------------------------------------------------------------------------
// §5.4 magnification controller
// ---------------------------------------------------------------------------

void test_magnification_controller() {
    RecordingBus bus;
    CHECK_THROWS(mag::MagnificationController(bus, mk(0, 0, 0, 0)), std::invalid_argument);

    mag::MagnificationController m(bus, mk(0, 0, 1920, 1080));
    CHECK_THROWS(m.set_factor(0), std::out_of_range);
    CHECK_THROWS(m.set_factor(mag::kFactorMin - 1), std::out_of_range);
    CHECK_THROWS(m.set_factor(mag::kFactorMax + 1), std::out_of_range);
    m.set_factor(mag::kQ16One * 2);
    CHECK(m.current().factor_q16 == mag::kQ16One * 2);

    // An axis the caller moves out of range is refused. The ratio lock derives
    // the *other* axis -- which is then legal by construction -- so a pair that
    // moves both is judged on the width, the one the rule treats as
    // authoritative; the height-only case below is judged on the height.
    CHECK_THROWS(m.resize_output(mag::SizePx{119, 500}), std::out_of_range);
    CHECK_THROWS(m.resize_output(mag::SizePx{320, mag::kMinOutputEdgePx - 1}), std::out_of_range);
    CHECK_THROWS(m.resize_output(mag::SizePx{2000, 500}), std::out_of_range);
    m.resize_output(mag::SizePx{640, 480});
    CHECK(m.current().output_size_px == (mag::SizePx{640, 480}));

    // Resizing holds the window's middle where it is. Growing from a corner --
    // which is what leaving the position alone amounts to -- walks the window
    // across the screen, and the size slider would drag it away from whatever
    // the user was looking at.
    m.center_on(mk(0, 0, 1920, 1080));
    m.resize_output(mag::SizePx{640, 480});
    const mag::RectPx before_grow = m.output_rect();
    m.resize_output(mag::SizePx{800, 600});
    const mag::RectPx grown = m.output_rect();
    CHECK(mag::width_of(grown) == 800);
    CHECK(mag::height_of(grown) == 600);
    CHECK(grown.left + mag::width_of(grown) / 2 ==
          before_grow.left + mag::width_of(before_grow) / 2);
    CHECK(grown.top + mag::height_of(grown) / 2 ==
          before_grow.top + mag::height_of(before_grow) / 2);

    // The stepped resize the arrow keys drive behaves the same way.
    const mag::RectPx before_step = m.output_rect();
    m.step_output_size(64, 0);
    const mag::RectPx stepped = m.output_rect();
    CHECK(mag::width_of(stepped) == mag::width_of(before_step) + 64);
    CHECK(stepped.left + mag::width_of(stepped) / 2 ==
          before_step.left + mag::width_of(before_step) / 2);

    // Shrinking too, and back to where it started.
    m.step_output_size(-64, 0);
    const mag::RectPx shrunk = m.output_rect();
    CHECK(mag::width_of(shrunk) == mag::width_of(before_step));
    CHECK(shrunk.left == before_step.left);

    // The zoom ladder is monotone in both directions.
    Q16 f = mag::kQ16One;
    for (int i = 0; i < 5; ++i) {
        const Q16 next = mag::ladder_step(f, 1);
        CHECK(next > f);
        f = next;
    }
    for (int i = 0; i < 5; ++i) {
        const Q16 next = mag::ladder_step(f, -1);
        CHECK(next < f);
        f = next;
    }
}

// ---------------------------------------------------------------------------
// §3.3 interaction state machine
// ---------------------------------------------------------------------------

void test_interaction_state_machine() {
    RecordingBus bus;
    FakeOverlay overlay;
    FakeCapture capture;
    mag::InteractionStateMachine sm(bus, overlay, capture);

    CHECK(sm.state() == mag::InteractionState::Off);
    CHECK(sm.request_toggle() == mag::InteractionState::Interactive);
    CHECK(overlay.shown);
    CHECK(capture.starts >= 1);
    CHECK(!overlay.hit_transparent);
    CHECK(sm.wants_hit_test());

    CHECK(sm.request_pass_through(true) == mag::InteractionState::PassThrough);
    CHECK(overlay.hit_transparent);
    CHECK(!sm.wants_hit_test());
    CHECK(sm.request_pass_through(false) == mag::InteractionState::Interactive);
    CHECK(!overlay.hit_transparent);

    CHECK(sm.on_capture_lost() == mag::InteractionState::Suspended);
    CHECK(sm.resume_state() == mag::InteractionState::Interactive);
    CHECK(sm.on_capture_recovered() == mag::InteractionState::Interactive);

    CHECK(sm.request_close() == mag::InteractionState::Off);
    CHECK(!overlay.shown);
    CHECK(capture.stops >= 1);
}

// ---------------------------------------------------------------------------
// Saved selections
// ---------------------------------------------------------------------------

void test_selection_slots() {
    const mag::RectPx desktop = mk(0, 0, 1920, 1080);
    mag::AppConfig cfg = mag::AppConfig::defaults();

    // Every slot starts empty, and validate() fills them from the selection in
    // force rather than leaving a rectangle in the corner.
    for (const mag::SelectionConfig& slot : cfg.selection_slots) {
        CHECK(slot.bounds_px == (mag::RectPx{0, 0, 0, 0}));
    }
    cfg.selection_bounds_px = mk(400, 300, 800, 600);
    cfg.selection_shape = mag::SelectionShape::Circle;
    cfg.selection_corner_radius_px = 24;
    CHECK(mag::validate(cfg, desktop));
    for (const mag::SelectionConfig& slot : cfg.selection_slots) {
        CHECK(slot.bounds_px == (mag::RectPx{400, 300, 800, 600}));
        CHECK(slot.shape == mag::SelectionShape::Circle);
        CHECK(slot.corner_radius_px == 24);
    }

    // A slot off the desktop is pulled back onto it, exactly as the selection is.
    cfg.selection_slots[1].bounds_px = mk(1800, 1000, 2200, 1200);
    CHECK(mag::validate(cfg, desktop));
    CHECK(cfg.selection_slots[1].bounds_px.right <= 1920);
    CHECK(cfg.selection_slots[1].bounds_px.bottom <= 1080);

    // The corner radius never exceeds what the slot's own bounds allow.
    cfg.selection_slots[2].bounds_px = mk(0, 0, 20, 20);
    cfg.selection_slots[2].corner_radius_px = 500;
    CHECK(mag::validate(cfg, desktop));
    CHECK(cfg.selection_slots[2].corner_radius_px <= 10);

    // An index outside the table is not a slot, and says so.
    cfg.selection_slot = 9;
    CHECK(mag::validate(cfg, desktop));
    CHECK(cfg.selection_slot == -1);

    // Round trip: what is written is what comes back.
    cfg.selection_slot = 2;
    cfg.selection_slots[3] = mag::SelectionConfig{mk(10, 20, 330, 260),
                                                  mag::SelectionShape::Ellipse, 12};
    const std::string text = mag::to_json(cfg);
    mag::AppConfig back = mag::AppConfig::defaults();
    CHECK(mag::from_json(text, back));
    CHECK(back.selection_slot == 2);
    CHECK(back.selection_slots[3].bounds_px == (mag::RectPx{10, 20, 330, 260}));
    CHECK(back.selection_slots[3].shape == mag::SelectionShape::Ellipse);
    CHECK(back.selection_slots[3].corner_radius_px == 12);
    CHECK(back.selection_slots[0].bounds_px == cfg.selection_slots[0].bounds_px);

    // A file written before the slots existed still loads, and the slots come
    // back filled from the selection rather than empty.
    mag::AppConfig older = mag::AppConfig::defaults();
    CHECK(mag::from_json(
        "{\"version\":1,\"selection_bounds_px\":[100,100,420,340],"
        "\"selection_shape\":\"Ellipse\",\"factor_q16\":262144}",
        older));
    CHECK(older.selection_bounds_px == (mag::RectPx{100, 100, 420, 340}));
    CHECK(older.selection_slot == -1);
    for (const mag::SelectionConfig& slot : older.selection_slots) {
        CHECK(mag::is_empty(mag::normalize(slot.bounds_px)));
    }
    CHECK(mag::validate(older, desktop));
    CHECK(older.selection_slots[0].bounds_px == (mag::RectPx{100, 100, 420, 340}));
    CHECK(older.selection_slots[0].shape == mag::SelectionShape::Ellipse);
}

// ---------------------------------------------------------------------------
// §5.4 the viewport: the factor is the magnification, the window is a window
// ---------------------------------------------------------------------------

void test_viewport() {
    const mag::SelectionConfig sel{mk(100, 200, 420, 440),  // 320x240
                                   mag::SelectionShape::Rectangle, 0};
    mag::MagnificationConfig cfg;
    cfg.factor_q16 = mag::kQ16One * 4;

    // At the size the factor asks for, the viewport is exactly the selection:
    // nothing is cropped and nothing outside it is shown.
    cfg.output_size_px = mag::SizePx{1280, 960};
    mag::ViewportMapping vp = mag::compute_viewport(sel, cfg);
    CHECK(vp.valid);
    CHECK(vp.applied_scale_q16 == mag::kQ16One * 4);
    CHECK(vp.dest_rect_px == (mag::RectPx{0, 0, 1280, 960}));
    CHECK(vp.src_sub_rect_px == (mag::RectPx{0, 0, 320, 240}));

    // A window twice that shape shows twice the desktop, still at 4x. This is
    // the whole point: the picture does not grow with the window, so the factor
    // box cannot be contradicted by a resize.
    cfg.output_size_px = mag::SizePx{2560, 1920};
    vp = mag::compute_viewport(sel, cfg);
    CHECK(vp.applied_scale_q16 == mag::kQ16One * 4);
    CHECK(mag::width_of(vp.src_sub_rect_px) == 640);
    CHECK(mag::height_of(vp.src_sub_rect_px) == 480);
    // Centred on the selection: 160 source pixels of desktop on either side.
    CHECK(vp.src_sub_rect_px.left == -160);
    CHECK(vp.src_sub_rect_px.top == -120);

    // A window smaller than the region crops it, and still does not zoom it.
    cfg.output_size_px = mag::SizePx{640, 480};
    vp = mag::compute_viewport(sel, cfg);
    CHECK(vp.applied_scale_q16 == mag::kQ16One * 4);
    CHECK(vp.src_sub_rect_px == (mag::RectPx{80, 60, 240, 180}));

    // A window one pixel over an exact multiple still magnifies by the factor:
    // the visible extent is floored, so the extra pixel is trimmed rather than
    // changing the scale.
    cfg.output_size_px = mag::SizePx{1281, 961};
    vp = mag::compute_viewport(sel, cfg);
    CHECK(vp.applied_scale_q16 == mag::kQ16One * 4);
    CHECK(mag::width_of(vp.src_sub_rect_px) == 320);
    CHECK(mag::height_of(vp.src_sub_rect_px) == 240);

    // The scale is the factor for every window size, a non-integer factor
    // included, and the visible extent is always what fills the window.
    const Q16 factors[] = {mag::kQ16One, mag::q16_from_double(1.25),
                           mag::kQ16One * 2, mag::q16_from_double(3.5),
                           mag::kFactorMax};
    const mag::SizePx windows[] = {mag::SizePx{120, 120}, mag::SizePx{640, 480},
                                   mag::SizePx{1280, 960}, mag::SizePx{1920, 1080},
                                   mag::SizePx{1000, 400}};
    for (const Q16 factor : factors) {
        cfg.factor_q16 = factor;
        for (const mag::SizePx& size : windows) {
            cfg.output_size_px = size;
            vp = mag::compute_viewport(sel, cfg);
            CHECK(vp.applied_scale_q16 == factor);
            // The visible source, scaled back up, covers the window to within
            // the rounding of one source pixel.
            const Px covered_w = mag::scale_px(mag::width_of(vp.src_sub_rect_px), factor);
            const Px covered_h = mag::scale_px(mag::height_of(vp.src_sub_rect_px), factor);
            CHECK(covered_w <= size.width);
            CHECK(covered_h <= size.height);
            CHECK(covered_w + mag::scale_px(1, factor) > size.width);
            CHECK(covered_h + mag::scale_px(1, factor) > size.height);
            // And it stays centred on the region, whatever the window is doing.
            const Px centre_x = 2 * vp.src_sub_rect_px.left + mag::width_of(vp.src_sub_rect_px);
            const Px centre_y = 2 * vp.src_sub_rect_px.top + mag::height_of(vp.src_sub_rect_px);
            CHECK(centre_x == 320 || centre_x == 319);
            CHECK(centre_y == 240 || centre_y == 239);
        }
    }

    // A factor outside the supported range is clamped rather than thrown on:
    // compute_viewport() is noexcept and must answer with something drawable.
    cfg.factor_q16 = mag::kFactorMax + mag::kQ16One;
    cfg.output_size_px = mag::SizePx{640, 480};
    vp = mag::compute_viewport(sel, cfg);
    CHECK(vp.valid);
    CHECK(vp.applied_scale_q16 == mag::kFactorMax);

    // Degenerate input is reported rather than divided by.
    mag::MagnificationConfig empty_size;
    empty_size.output_size_px = mag::SizePx{0, 0};
    CHECK(!mag::compute_viewport(sel, empty_size).valid);
    mag::SelectionConfig empty_sel{mk(10, 10, 10, 10), mag::SelectionShape::Rectangle, 0};
    cfg.output_size_px = mag::SizePx{640, 480};
    CHECK(!mag::compute_viewport(empty_sel, cfg).valid);
}

// ---------------------------------------------------------------------------
// §3.3 hotkey text: describe and parse are inverses, for every chord
// ---------------------------------------------------------------------------

void test_hotkey_text() {
    // Every shipped chord has to survive a round trip through its own text:
    // the field shows what describe_chord() says, and the file it writes is
    // that same text, so a name that cannot be parsed back is a hotkey that
    // disappears on the next load.
    const mag::AppConfig defaults = mag::AppConfig::defaults();
    for (std::size_t i = 0; i < static_cast<std::size_t>(mag::HotkeyAction::Count); ++i) {
        const mag::HotkeyChord& chord = defaults.hotkeys[i];
        CHECK(mag::chord_is_bound(chord));
        const std::string text = mag::describe_chord(chord);
        mag::HotkeyChord back{};
        CHECK(mag::parse_chord(text, back));
        CHECK(back == chord);
        CHECK(!mag::chord_is_mouse_button(chord));
    }

    // An unbound chord has no text, and the empty string parses back to it.
    CHECK(mag::describe_chord(mag::HotkeyChord{}).empty());
    mag::HotkeyChord unbound{1, 2};
    CHECK(mag::parse_chord("", unbound));
    CHECK(!mag::chord_is_bound(unbound));

    // The mouse buttons: what the capture stores, what the field shows, and
    // what a hand-edited file may call them.
    struct MouseName {
        const char* text;
        std::uint32_t vk;
    };
    const MouseName names[] = {
        {"MouseRight", 0x02}, {"MouseMiddle", 0x04}, {"Mouse4", 0x05}, {"Mouse5", 0x06},
        {"MouseBack", 0x05},  {"MouseForward", 0x06}, {"XButton1", 0x05}, {"XButton2", 0x06},
        {"MMB", 0x04},        {"RMB", 0x02},
    };
    for (const MouseName& entry : names) {
        mag::HotkeyChord chord{};
        CHECK(mag::parse_chord(entry.text, chord));
        CHECK(chord.virtual_key == entry.vk);
        CHECK(chord.modifiers == 0);
        CHECK(mag::chord_is_bound(chord));
        CHECK(mag::chord_is_mouse_button(chord));
        // And back out again, under the canonical name.
        const std::string canonical = mag::describe_chord(chord);
        CHECK(!canonical.empty());
        mag::HotkeyChord again{};
        CHECK(mag::parse_chord(canonical, again));
        CHECK(again == chord);
    }
    CHECK(mag::describe_chord(mag::HotkeyChord{0, 0x05}) == "Mouse4");
    CHECK(mag::describe_chord(mag::HotkeyChord{0, 0x06}) == "Mouse5");
    CHECK(mag::describe_chord(mag::HotkeyChord{0, 0x04}) == "MouseMiddle");
    CHECK(mag::describe_chord(mag::HotkeyChord{1, 0x05}) == "Alt+Mouse4");

    // The keyboard is not the mouse, and a key that shares a number with a
    // mouse button must not be read as one.
    CHECK(!mag::is_mouse_button_vk(0x00));
    CHECK(!mag::is_mouse_button_vk(0x08));   // Backspace
    CHECK(!mag::is_mouse_button_vk('M'));
    for (std::uint32_t vk = 0x07; vk <= 0xFF; ++vk) {
        CHECK(!mag::is_mouse_button_vk(vk));
    }

    // A mouse chord survives the configuration file too: it is written as a
    // number pair and read back as itself.
    mag::AppConfig cfg = mag::AppConfig::defaults();
    cfg.hotkeys[static_cast<std::size_t>(mag::HotkeyAction::CycleShape)] =
        mag::HotkeyChord{0, 0x05};
    mag::AppConfig back = mag::AppConfig::defaults();
    CHECK(mag::from_json(mag::to_json(cfg), back));
    CHECK(back.hotkeys[static_cast<std::size_t>(mag::HotkeyAction::CycleShape)] ==
          (mag::HotkeyChord{0, 0x05}));
    CHECK(mag::to_json(cfg).find("Mouse4") == std::string::npos);  // numbers, not names
}

// ---------------------------------------------------------------------------
// §5.4 the ratio lock on every resize path
// ---------------------------------------------------------------------------

void test_ratio_lock() {
    RecordingBus bus;
    mag::MagnificationController m(bus, mk(0, 0, 1920, 1440));

    // Start from a 4:3 window with the lock on (the shipped default).
    m.resize_output(mag::SizePx{1280, 960});
    CHECK(m.current().keep_aspect_ratio);
    CHECK(m.current().output_size_px == (mag::SizePx{1280, 960}));

    // Changing one axis moves the other: that is what "keep the ratio" means,
    // and it has to hold however the resize arrived -- a typed width, a stepped
    // hotkey, a dragged slider or an edge drag all come through here.
    m.resize_output(mag::SizePx{800, 960});
    CHECK(m.current().output_size_px == (mag::SizePx{800, 600}));
    m.resize_output(mag::SizePx{800, 1200});
    CHECK(m.current().output_size_px == (mag::SizePx{1600, 1200}));

    // A size that moved both axes keeps the width and derives the height.
    m.resize_output(mag::SizePx{1000, 1000});
    CHECK(m.current().output_size_px == (mag::SizePx{1000, 750}));

    // The arrow hotkeys step one axis and the other follows.
    m.resize_output(mag::SizePx{1000, 750});
    m.step_output_size(64, 0);
    CHECK(m.current().output_size_px == (mag::SizePx{1064, 798}));
    m.step_output_size(0, 40);
    CHECK(m.current().output_size_px == (mag::SizePx{1117, 838}));   // 798 + 40, width derived

    // Undoing a step lands back where it started: the derivation rounds rather
    // than truncating, so a round trip cannot drift a pixel at a time.
    const mag::SizePx before = m.current().output_size_px;
    m.step_output_size(64, 0);
    m.step_output_size(-64, 0);
    CHECK(m.current().output_size_px == before);

    // At the desktop ceiling the derived side clamps and the moved side comes
    // back with it, so the window stops growing instead of going lopsided. A
    // square window asked to be 1920 wide wants to be 1920 tall, and the
    // desktop is 1440: both come back as 1440.
    m.set_keep_aspect_ratio(false);
    m.resize_output(mag::SizePx{1000, 1000});
    m.set_keep_aspect_ratio(true);
    m.resize_output(mag::SizePx{1920, 1000});
    CHECK(m.current().output_size_px == (mag::SizePx{1440, 1440}));

    // With the lock off, one axis moves alone. That is the whole difference
    // between the two settings.
    m.set_keep_aspect_ratio(false);
    m.resize_output(mag::SizePx{1280, 960});
    m.resize_output(mag::SizePx{800, 960});
    CHECK(m.current().output_size_px == (mag::SizePx{800, 960}));
    m.resize_output(mag::SizePx{800, 1200});
    CHECK(m.current().output_size_px == (mag::SizePx{800, 1200}));

    // A resize that asks for nothing keeps the shape it has, and an
    // out-of-range primary axis is still refused rather than clamped into
    // something else: that is the documented contract of resize_output().
    m.set_keep_aspect_ratio(false);
    m.resize_output(mag::SizePx{1024, 768});
    m.set_keep_aspect_ratio(true);
    m.resize_output(mag::SizePx{1024, 768});
    CHECK(m.current().output_size_px == (mag::SizePx{1024, 768}));
    CHECK_THROWS(m.resize_output(mag::SizePx{19, 768}), std::out_of_range);
    CHECK_THROWS(m.resize_output(mag::SizePx{1024, 19}), std::out_of_range);
    CHECK(m.current().output_size_px == (mag::SizePx{1024, 768}));

    // The ratio is the *window's*, not the selection's: fitting the window to
    // the region at a factor sets an exact size and is deliberately not locked,
    // or a region whose proportions differ from the window's could never be
    // shown whole.
    const mag::SelectionConfig sel{mk(0, 0, 320, 180), mag::SelectionShape::Rectangle, 0};
    m.set_factor(mag::kQ16One * 4);
    m.fit_output_to_selection(sel);
    CHECK(m.current().output_size_px == (mag::SizePx{1280, 720}));
}

}  // namespace

int main() {
    std::printf("=== magnifier core tests ===\n");
    test_shapes();
    test_handles();
    test_corner_radius_and_conform();
    test_fixed_point();
    test_event_bus();
    test_selection_controller();
    test_selection_drag();
    test_random_round_trips();
    test_magnification_controller();
    test_viewport();
    test_ratio_lock();
    test_hotkey_text();
    test_selection_slots();
    test_interaction_state_machine();

    if (g_failures == 0) {
        std::printf("all %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
