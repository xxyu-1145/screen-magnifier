// core/interaction_state_machine.h — Off / Interactive / PassThrough /
// EdgeArmed / Suspended, exactly as specified in design doc §3.3.
#pragma once

#include "core/event_bus.h"
#include "core/types.h"

namespace mag {

// The window operations the state machine needs. Implemented by
// platform::OverlayWindow so that core/ stays free of Win32.
class IOverlayWindow {
public:
    virtual ~IOverlayWindow() = default;

    virtual void show() noexcept = 0;
    virtual void hide() noexcept = 0;

    // transparent == true makes the window click-through: the desktop below
    // keeps receiving the mouse (requirement 7).
    virtual void set_hit_test_transparent(bool transparent) = 0;

    // Draws the "you can interact again" affordance. Never steals focus.
    virtual void show_edge_hint() noexcept = 0;
    virtual void hide_edge_hint() noexcept = 0;
};

// The capture lifecycle operations the state machine drives.
class ICaptureControl {
public:
    virtual ~ICaptureControl() = default;

    virtual void start() noexcept = 0;
    virtual void stop() noexcept = 0;
    // Rebuild sessions after device removal or a topology change.
    virtual void recover_async() noexcept = 0;
};

class InteractionStateMachine {
public:
    InteractionStateMachine(IEventBus& bus, IOverlayWindow& overlay,
                            ICaptureControl& capture) noexcept;

    // Consumes one event and applies the transition. Unhandled events are
    // ignored (they are normal: most events are not input events).
    InteractionState dispatch(const AppEvent& event) noexcept;

    // Explicit requests from the settings window / tray menu.
    InteractionState request_toggle() noexcept;
    InteractionState request_pass_through(bool enable) noexcept;
    InteractionState request_close() noexcept;

    // Edge-dwell tick from the input thread while in PassThrough.
    InteractionState on_edge_dwell(std::uint64_t dwell_ms, int threshold_ms) noexcept;

    // Raised by the input thread when the user starts dragging on the window
    // while it is only "armed".
    InteractionState on_window_interaction() noexcept;

    InteractionState on_capture_lost() noexcept;
    InteractionState on_capture_recovered() noexcept;

    InteractionState state() const noexcept { return state_; }

    // The state to restore once a suspended session recovers.
    InteractionState resume_state() const noexcept { return resume_state_; }

    // Whether the window should currently accept mouse input.
    bool wants_hit_test() const noexcept;

private:
    InteractionState transition_to(InteractionState next) noexcept;

    IEventBus& bus_;
    IOverlayWindow& overlay_;
    ICaptureControl& capture_;
    InteractionState state_{InteractionState::Off};
    InteractionState resume_state_{InteractionState::Interactive};
    InteractionState state_before_suspend_{InteractionState::Interactive};
};

}  // namespace mag
