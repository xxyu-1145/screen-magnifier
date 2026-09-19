// core/events.h — the complete set of messages that cross a thread boundary.
//
// Constraint (design doc §4.3): every event is a copyable value object that
// carries a timestamp and the id of the thread that produced it. Nothing here
// may hold a bare pointer or reference into mutable shared state.
#pragma once

#include <cstdint>
#include <variant>

#include "core/types.h"

namespace mag {

// Stable ids for the hotkeys the app can bind (design doc §3.3).
enum class HotkeyAction : std::uint32_t {
    ToggleMagnifier = 0,   // show/hide the magnifier
    TogglePassThrough,     // switch Interactive <-> PassThrough
    CycleShape,            // rectangle -> circle -> ellipse -> rounded
    ZoomIn,
    ZoomOut,
    Preset1,
    Preset2,
    Preset3,
    Preset4,
    // The four pixel-nudge actions were removed: moving the region a pixel at a
    // time proved less useful than dragging it, and they cost four of the
    // limited number of chords a user is willing to remember.
    GrowWidth,             // output window resize in steps
    ShrinkWidth,
    GrowHeight,
    ShrinkHeight,
    ResetSelection,
    // Puts the magnifier window back in the middle of the primary screen. The
    // selection has had its own reset all along; this is the same gesture for
    // the window, which is the thing that actually wanders.
    CenterOutput,
    Quit,
    Count,
};

inline constexpr std::size_t kHotkeyCount = static_cast<std::size_t>(HotkeyAction::Count);

const char* hotkey_action_name(HotkeyAction a) noexcept;
bool hotkey_action_from_name(const char* name, HotkeyAction& out) noexcept;

// A Win32 modifier mask (MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_WIN) plus a virtual
// key code. Kept as plain integers so core/ stays free of <windows.h>.
struct HotkeyChord {
    std::uint32_t modifiers{0};
    std::uint32_t virtual_key{0};

    friend constexpr bool operator==(const HotkeyChord&, const HotkeyChord&) = default;
};

constexpr bool chord_is_bound(const HotkeyChord& c) noexcept {
    return c.virtual_key != 0;
}

// ---------------------------------------------------------------------------
// Event payloads
// ---------------------------------------------------------------------------

struct SelectionChanged {
    SelectionConfig value;
};

struct StateChanged {
    InteractionState value;
};

struct HotkeyPressed {
    std::uint32_t id;  // HotkeyAction
};

// A full immutable render state. Published by the control layer whenever the
// selection, magnification, window size or interaction state changes.
struct SnapshotChanged {
    RenderSnapshot value;
};

struct CaptureLost {
    std::uint32_t output_id;
};

struct CaptureRecovered {
    std::uint32_t output_id;
};

struct TopologyChanged {
    std::uint64_t revision;
};

struct RenderFault {
    std::uint32_t code;
};

// Raised for anything the user must be told about: capture refused, device
// removed, backend switch failed (design doc §6.2). The message is owned by the
// event because the renderer reports diagnostics it builds at runtime, and a
// bare pointer crossing a thread boundary is exactly what §4.3 forbids.
struct ErrorReported {
    std::uint32_t code;
    std::string text;
};

// Cursor sampling tick produced by the input thread. Drives edge-dwell
// recovery without installing a low-level mouse hook.
struct CursorSampled {
    PointPx cursor_px;
    std::uint64_t dwell_ms;  // how long the cursor has been on the same edge
};

// A user action originating in the settings window or the tray menu.
struct CommandIssued {
    std::uint32_t id;
};

struct QuitRequested {};

using Payload = std::variant<SelectionChanged,
                             StateChanged,
                             HotkeyPressed,
                             SnapshotChanged,
                             CaptureLost,
                             CaptureRecovered,
                             TopologyChanged,
                             RenderFault,
                             ErrorReported,
                             CursorSampled,
                             CommandIssued,
                             QuitRequested>;

// Thin metadata wrapper so that `publish(const AppEvent&)` matches the
// interface in the design doc while still carrying §4.3's required fields.
struct AppEvent {
    Payload payload;
    std::uint64_t timestamp_qpc{0};
    std::uint32_t source_thread{0};

    template <typename T>
    static AppEvent make(T value) {
        AppEvent e;
        e.payload = std::move(value);
        return e;
    }
};

// Convenience constructors used by producers.
AppEvent make_event(Payload payload) noexcept;
AppEvent make_event(Payload payload, std::uint64_t timestamp_qpc, std::uint32_t source_thread) noexcept;

}  // namespace mag
