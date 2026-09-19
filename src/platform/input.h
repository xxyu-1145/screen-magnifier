// platform/input.h — the input thread: global hotkeys plus cursor sampling.
//
// Design doc §3.3: RegisterHotKey is the default and recommended path. The
// low-level keyboard hook is only a fallback for the chords RegisterHotKey
// could not have -- the ones another application already owns -- so a hotkey
// never silently does nothing, and a chord both paths handled can not fire
// twice. Strict compatibility mode refuses the hook outright, which is the user
// asking for RegisterHotKey and nothing else. The mouse edge-dwell detector
// never uses a hook at all: it polls GetCursorPos on this thread.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/event_bus.h"
#include "core/events.h"
#include "core/types.h"

namespace mag {

class InputThread {
public:
    explicit InputThread(BoundedEventBus& bus);
    ~InputThread();

    InputThread(const InputThread&) = delete;
    InputThread& operator=(const InputThread&) = delete;

    // Starts the thread and registers the supplied chords. Returns false if
    // the thread could not be created.
    bool start(const std::array<HotkeyChord, kHotkeyCount>& chords, bool strict_compat);

    // Stops the thread, unregistering every hotkey and removing any hook.
    void stop();

    // Re-registers from scratch (config changed). `out_conflicts` receives the
    // actions whose chord could not be registered because another application
    // already owns it.
    void set_hotkeys(const std::array<HotkeyChord, kHotkeyCount>& chords,
                     std::vector<HotkeyAction>& out_conflicts);

    // Enables or disables the WH_KEYBOARD_LL fallback. Refused (returns false)
    // while strict compatibility mode is on.
    bool set_low_level_fallback(bool enabled);

    // Turns strict compatibility mode on or off after the thread has started.
    // It is the flag that refuses the hook, so it cannot live only in start().
    void set_strict_compat(bool strict);

    // Releases every chord -- and the fallback hook with it -- while the user is
    // typing a new one into the settings window, then puts both back. The chord
    // table is untouched, so resuming re-registers exactly what was in force.
    //
    // RegisterHotKey consumes the combinations it owns: without this, pressing a
    // chord the program already holds does nothing to the field and fires the
    // action instead, which is the one thing a rebind must not do.
    void set_registration_suspended(bool suspended);

    // Publishes the rectangle the edge-dwell detector watches, in virtual
    // desktop physical pixels. Disabled when `enabled` is false.
    void set_edge_zone(std::optional<RectPx> zone_px, Px band_px, bool enabled);

    // Human-readable diagnosis of the last chord that failed to register.
    std::string registration_report() const;

private:
    void thread_main();
    void register_all(const std::array<HotkeyChord, kHotkeyCount>& chords,
                      std::vector<HotkeyAction>& out_conflicts);
    void unregister_all();
    // Installs or removes the WH_KEYBOARD_LL fallback and reports whether it is
    // in force afterwards. Input thread only: a hook has to be installed by the
    // thread that pumps messages.
    bool apply_low_level(bool enabled);
    void poll_cursor();

    BoundedEventBus& bus_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    DWORD thread_id_{0};

    mutable std::mutex mutex_;
    std::array<HotkeyChord, kHotkeyCount> chords_{};
    std::vector<HotkeyAction> registered_;
    std::string report_;

    // Edge-dwell state, touched only on the input thread.
    std::optional<RectPx> edge_zone_px_;
    Px edge_band_px_{8};
    bool edge_enabled_{false};
    bool edge_inside_{false};
    std::uint64_t edge_since_ms_{0};
    bool edge_fired_{false};

    // Low-level fallback bookkeeping.
    static LRESULT CALLBACK ll_keyboard_proc(int code, WPARAM wparam, LPARAM lparam);
    // Mouse buttons cannot be registered at all, so a chord bound to one of
    // them is served here: this is the only path that sees a mouse button
    // outside the window under the cursor.
    static LRESULT CALLBACK ll_mouse_proc(int code, WPARAM wparam, LPARAM lparam);
    static InputThread* ll_owner_;
    HHOOK ll_hook_{nullptr};
    HHOOK ll_mouse_hook_{nullptr};
    std::atomic<bool> ll_enabled_{false};
    // Whether the hook was in force when the chords were released for a rebind,
    // so resuming restores the arrangement the user actually had. Input thread
    // only.
    bool ll_resume_{false};
    bool suspended_{false};
};

}  // namespace mag
