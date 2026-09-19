// core/interaction_state_machine.cpp — the §3.3 state table.
//
// Two properties of the frozen header shape everything below:
//   * every applied transition publishes StateChanged, so the overlay, the
//     settings window and the render thread never have to poll for the mode;
//   * no entry point throws. The design doc raises std::logic_error for an
//     unknown combination, but the header declares those methods noexcept and
//     asks for "leave the state alone" instead, which is also what a hotkey
//     storm needs.
#include "core/interaction_state_machine.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <variant>

namespace mag {
namespace {

// §3.3 pins the edge dwell at 500 ms. dispatch() only ever sees a CursorSampled
// that carries the elapsed dwell, not the threshold, so the default lives here;
// the input thread can still pass its configured value through on_edge_dwell().
constexpr std::uint64_t kEdgeDwellThresholdMs = 500;

// ErrorReported::text is a bare const char*, so the event may only ever point at
// a literal that outlives the queue.
constexpr const char* kHitTestFaultText = "overlay hit-test update failed";
constexpr std::uint32_t kHitTestFaultCode = 2;

// overlay.set_hit_test_transparent() is allowed to throw std::system_error
// (§5.6) while every entry point of this class is noexcept. §5.1 lets the
// boundary layer turn such a failure into an error event, and doing that is the
// only option that neither terminates the process nor swallows the fault.
void apply_hit_test(IEventBus& bus, IOverlayWindow& overlay, bool transparent) noexcept {
    try {
        overlay.set_hit_test_transparent(transparent);
    } catch (const std::exception&) {
        bus.publish(AppEvent::make(ErrorReported{kHitTestFaultCode, kHitTestFaultText}));
    } catch (...) {
        bus.publish(AppEvent::make(ErrorReported{kHitTestFaultCode, kHitTestFaultText}));
    }
}

// Side effects shared by every path that lands in Interactive. Hit testing is
// asserted rather than assumed: leaving PassThrough for Off and back would
// otherwise re-show a window that is still click-through while the state
// machine claims the user can grab it.
void enter_interactive(IEventBus& bus, IOverlayWindow& overlay, InteractionState from) noexcept {
    if (from == InteractionState::EdgeArmed) overlay.hide_edge_hint();
    apply_hit_test(bus, overlay, false);
}

void enter_pass_through(IEventBus& bus, IOverlayWindow& overlay, InteractionState from) noexcept {
    if (from == InteractionState::EdgeArmed) overlay.hide_edge_hint();
    apply_hit_test(bus, overlay, true);
}

void enter_off(IOverlayWindow& overlay, ICaptureControl& capture, InteractionState from) noexcept {
    if (from == InteractionState::EdgeArmed) overlay.hide_edge_hint();
    capture.stop();
    overlay.hide();
}

// The pass-through hotkey and the pinned button are a toggle, not a setter, and
// §3.3 sends EdgeArmed back to Interactive as well: the armed state is already
// "the window can be used", so the toggle must not silently re-arm click-through
// when the user presses it there.
InteractionState toggle_pass_through(InteractionStateMachine& machine) noexcept {
    switch (machine.state()) {
        case InteractionState::Interactive: return machine.request_pass_through(true);
        case InteractionState::PassThrough:
        case InteractionState::EdgeArmed: return machine.request_pass_through(false);
        case InteractionState::Off:
        case InteractionState::Suspended: return machine.state();
    }
    return machine.state();
}

struct EventVisitor {
    InteractionStateMachine& machine;

    InteractionState operator()(const HotkeyPressed& event) const noexcept {
        switch (static_cast<HotkeyAction>(event.id)) {
            case HotkeyAction::ToggleMagnifier: return machine.request_toggle();
            case HotkeyAction::TogglePassThrough: return toggle_pass_through(machine);
            case HotkeyAction::Quit: return machine.request_close();
            // Shape, zoom and nudge belong to the domain controllers; the state
            // machine has no opinion about them.
            default: return machine.state();
        }
    }

    InteractionState operator()(const CommandIssued& event) const noexcept {
        // Command ids arrive from the tray/menu enum space, which is not this
        // class's; the app layer translates the ones it cares about before
        // publishing, so an id outside the hotkey range is simply not addressed
        // to the state machine.
        if (event.id >= kHotkeyCount) return machine.state();
        return (*this)(HotkeyPressed{event.id});
    }

    InteractionState operator()(const CursorSampled& event) const noexcept {
        return machine.on_edge_dwell(event.dwell_ms, static_cast<int>(kEdgeDwellThresholdMs));
    }

    InteractionState operator()(const CaptureLost&) const noexcept {
        return machine.on_capture_lost();
    }

    InteractionState operator()(const CaptureRecovered&) const noexcept {
        return machine.on_capture_recovered();
    }

    InteractionState operator()(const QuitRequested&) const noexcept {
        return machine.request_close();
    }

    template <typename T>
    InteractionState operator()(const T&) const noexcept {
        return machine.state();
    }
};

}  // namespace

InteractionStateMachine::InteractionStateMachine(IEventBus& bus, IOverlayWindow& overlay,
                                                ICaptureControl& capture) noexcept
    : bus_(bus), overlay_(overlay), capture_(capture) {}

InteractionState InteractionStateMachine::transition_to(InteractionState next) noexcept {
    state_ = next;
    bus_.publish(AppEvent::make(StateChanged{next}));
    return next;
}

InteractionState InteractionStateMachine::dispatch(const AppEvent& event) noexcept {
    return std::visit(EventVisitor{*this}, event.payload);
}

InteractionState InteractionStateMachine::request_toggle() noexcept {
    switch (state_) {
        case InteractionState::Off:
            overlay_.show();
            capture_.start();
            enter_interactive(bus_, overlay_, state_);
            return transition_to(InteractionState::Interactive);
        case InteractionState::EdgeArmed:
            // §3.3: while armed the toggle key only cancels the hint. It must
            // not hide the magnifier the user is reaching for.
            enter_interactive(bus_, overlay_, state_);
            return transition_to(InteractionState::Interactive);
        case InteractionState::Interactive:
        case InteractionState::PassThrough:
        case InteractionState::Suspended:
            // Undefined by the table, so it is not our event: AppHost routes
            // the hide request through request_close().
            return state_;
    }
    return state_;
}

InteractionState InteractionStateMachine::request_pass_through(bool enable) noexcept {
    if (enable) {
        // Off and Suspended have no window to make click-through, and a second
        // request in PassThrough is already satisfied.
        if (state_ == InteractionState::Interactive || state_ == InteractionState::EdgeArmed) {
            enter_pass_through(bus_, overlay_, state_);
            return transition_to(InteractionState::PassThrough);
        }
        return state_;
    }
    if (state_ == InteractionState::PassThrough || state_ == InteractionState::EdgeArmed) {
        enter_interactive(bus_, overlay_, state_);
        return transition_to(InteractionState::Interactive);
    }
    return state_;
}

InteractionState InteractionStateMachine::request_close() noexcept {
    if (state_ == InteractionState::Off) return state_;  // nothing left to release
    enter_off(overlay_, capture_, state_);
    return transition_to(InteractionState::Off);
}

InteractionState InteractionStateMachine::on_edge_dwell(std::uint64_t dwell_ms,
                                                       int threshold_ms) noexcept {
    const bool reached =
        threshold_ms <= 0 || dwell_ms >= static_cast<std::uint64_t>(threshold_ms);

    if (state_ == InteractionState::EdgeArmed) {
        if (reached) return state_;  // still parked on the edge: keep the hint up
        // The cursor left the edge before the user grabbed the window, so the
        // magnifier becomes click-through again.
        enter_pass_through(bus_, overlay_, state_);
        return transition_to(InteractionState::PassThrough);
    }
    if (state_ != InteractionState::PassThrough || !reached) return state_;

    // Requirement 7: after the dwell the window must be draggable and resizable
    // again, so hit testing comes back. Nothing here activates or foregrounds
    // the window — the overlay draws the affordance and the keyboard focus stays
    // with whatever the user was working in.
    overlay_.show_edge_hint();
    apply_hit_test(bus_, overlay_, false);
    return transition_to(InteractionState::EdgeArmed);
}

InteractionState InteractionStateMachine::on_window_interaction() noexcept {
    // Only meaningful while armed: in Interactive the grab already worked, and
    // in PassThrough the window cannot be grabbed at all because the mouse never
    // reaches it.
    if (state_ != InteractionState::EdgeArmed) return state_;
    enter_interactive(bus_, overlay_, state_);
    return transition_to(InteractionState::Interactive);
}

InteractionState InteractionStateMachine::on_capture_lost() noexcept {
    if (state_ == InteractionState::Off || state_ == InteractionState::Suspended) return state_;

    state_before_suspend_ = state_;
    // §3.3 resumes into a user-facing mode only. An armed hint is transient and
    // the cursor has almost certainly left the edge by the time a backend is
    // rebuilt, so that case resumes as Interactive instead of re-asserting a
    // hint nobody asked for.
    resume_state_ = state_before_suspend_ == InteractionState::PassThrough
                        ? InteractionState::PassThrough
                        : InteractionState::Interactive;
    if (state_ == InteractionState::EdgeArmed) overlay_.hide_edge_hint();
    capture_.recover_async();
    return transition_to(InteractionState::Suspended);
}

InteractionState InteractionStateMachine::on_capture_recovered() noexcept {
    if (state_ != InteractionState::Suspended) return state_;
    // Recovery re-asserts the hit-test mode: the overlay may have been recreated
    // along with the capture sessions, so the window is not assumed to have kept
    // its transparency.
    if (resume_state_ == InteractionState::PassThrough) {
        enter_pass_through(bus_, overlay_, state_);
        return transition_to(InteractionState::PassThrough);
    }
    enter_interactive(bus_, overlay_, state_);
    return transition_to(InteractionState::Interactive);
}

bool InteractionStateMachine::wants_hit_test() const noexcept {
    switch (state_) {
        case InteractionState::Interactive:
        case InteractionState::EdgeArmed: return true;
        case InteractionState::Off:
        case InteractionState::PassThrough:
        case InteractionState::Suspended: return false;
    }
    return false;
}

}  // namespace mag
