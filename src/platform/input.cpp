// platform/input.cpp — the input thread: global hotkeys plus cursor sampling.
//
// Design doc §3.3: RegisterHotKey is the default path and the only one that is
// allowed while strict compatibility mode is on. WH_KEYBOARD_LL exists solely
// as a fallback for chords another application already owns, and the edge-dwell
// detector never installs a mouse hook at all — it samples GetCursorPos here.

#include "platform/input.h"

#include <chrono>
#include <condition_variable>
#include <utility>

namespace mag {
namespace {

// Hotkey ids handed to RegisterHotKey must stay inside 0x0000..0xBFFF. One
// contiguous block keeps the translation from WM_HOTKEY back to an action
// unambiguous and cannot collide with ids owned by other code in the process.
constexpr int kHotkeyBaseId = 0x4000;
constexpr UINT_PTR kEdgeTimerId = 1;
constexpr UINT kEdgePollMs = 50;  // requirement 7 measures dwell in 500 ms steps

// Thread messages: the input thread owns no window, so the owner thread talks
// to it with PostThreadMessage.
constexpr UINT kMsgReRegister = WM_APP + 0x101;
constexpr UINT kMsgSetLowLevel = WM_APP + 0x102;
constexpr UINT kMsgSuspend = WM_APP + 0x103;

// How long the owner thread waits for the input thread before giving up. Both
// operations are a handful of microsecond-scale Win32 calls, so a timeout only
// fires when the thread is wedged, and then returning beats hanging the UI.
constexpr auto kHandshakeTimeout = std::chrono::seconds(2);

// The app owns exactly one InputThread (app/app_host.h) and the frozen class
// layout has no room for a cross-thread channel, so the hand-off between the
// owner thread and the input thread lives here. Every field is guarded by
// g_channel_mutex; nothing else in the process may reference them.
std::mutex g_channel_mutex;
std::condition_variable g_channel_cv;
bool g_queue_ready = false;
std::array<HotkeyChord, kHotkeyCount> g_requested_chords{};
std::vector<HotkeyAction> g_requested_conflicts;
bool g_reregister_request = false;
bool g_ll_request = false;
bool g_ll_want = false;
bool g_ll_result = false;
bool g_suspend_request = false;
bool g_suspend_want = false;
bool g_strict_compat = false;
std::optional<RectPx> g_requested_zone;
Px g_requested_band = 0;
bool g_requested_edge_enabled = false;
bool g_edge_request = false;

// Written by register_all and read by the keyboard hook. Both run on the input
// thread — WH_KEYBOARD_LL callbacks are delivered to the thread that installed
// the hook — so the hook can consult this table without taking a lock, which is
// what keeps it from ever blocking the keyboard.
std::array<HotkeyChord, kHotkeyCount> g_ll_chords{};
std::array<HotkeyAction, kHotkeyCount> g_ll_actions{};
std::size_t g_ll_chord_count = 0;
UINT_PTR g_edge_timer = 0;

int hotkey_id_for(std::size_t index) noexcept {
    return kHotkeyBaseId + static_cast<int>(index);
}

std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}

void publish_payload(BoundedEventBus& bus, Payload payload) noexcept {
    AppEvent event = AppEvent::make(std::move(payload));
    event.timestamp_qpc = qpc_now();
    event.source_thread = static_cast<std::uint32_t>(GetCurrentThreadId());
    bus.publish(event);
}

// Exact modifier match, the same rule RegisterHotKey applies: a chord bound to
// Ctrl+M must not also fire for Ctrl+Alt+M.
bool chord_modifiers_match(std::uint32_t mask) noexcept {
    const bool want_ctrl = (mask & MOD_CONTROL) != 0;
    const bool want_alt = (mask & MOD_ALT) != 0;
    const bool want_shift = (mask & MOD_SHIFT) != 0;
    const bool want_win = (mask & MOD_WIN) != 0;

    const bool have_ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool have_alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool have_shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool have_win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;

    return have_ctrl == want_ctrl && have_alt == want_alt && have_shift == want_shift &&
           have_win == want_win;
}

// The action bound to this key or button, when the fallback table holds a chord
// that matches it right now. Only the chords RegisterHotKey could not have live
// here -- the ones another application owns, and every mouse button -- so a
// match means a hook is the only path that will see this event.
std::optional<HotkeyAction> match_fallback_chord(std::uint32_t vk) noexcept {
    if (g_ll_chord_count == 0) return std::nullopt;
    for (std::size_t i = 0; i < g_ll_chord_count; ++i) {
        const HotkeyChord& chord = g_ll_chords[i];
        if (chord.virtual_key != vk) continue;
        if (!chord_modifiers_match(chord.modifiers)) continue;
        return g_ll_actions[i];
    }
    return std::nullopt;
}

// True when any of the fallback chords is a mouse button, which is what decides
// whether the mouse hook is worth installing.
bool fallback_has_mouse_chord() noexcept {
    for (std::size_t i = 0; i < g_ll_chord_count; ++i) {
        if (is_mouse_button_vk(g_ll_chords[i].virtual_key)) return true;
    }
    return false;
}

}  // namespace

InputThread* InputThread::ll_owner_ = nullptr;

InputThread::InputThread(BoundedEventBus& bus) : bus_(bus) {}

InputThread::~InputThread() {
    stop();
}

bool InputThread::start(const std::array<HotkeyChord, kHotkeyCount>& chords, bool strict_compat) {
    if (running_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chords_ = chords;
        registered_.clear();
        report_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_strict_compat = strict_compat;
        g_queue_ready = false;
        g_reregister_request = false;
        g_ll_request = false;
        g_edge_request = false;
    }
    running_.store(true);
    try {
        thread_ = std::thread(&InputThread::thread_main, this);
    } catch (...) {
        running_.store(false);
        return false;
    }
    // thread_id_ is written once by the input thread before it publishes
    // g_queue_ready, so this acquire makes the id safe to read.
    std::unique_lock<std::mutex> lock(g_channel_mutex);
    g_channel_cv.wait_for(lock, kHandshakeTimeout, [] { return g_queue_ready; });
    return true;
}

void InputThread::stop() {
    const bool was_running = running_.exchange(false);
    if (thread_.joinable()) {
        if (was_running) {
            std::unique_lock<std::mutex> lock(g_channel_mutex);
            // PostThreadMessage must not race the queue creation, or WM_QUIT
            // is lost and the thread stays blocked in GetMessage forever.
            g_channel_cv.wait_for(lock, kHandshakeTimeout, [] { return g_queue_ready; });
            if (thread_id_ != 0) PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        }
        thread_.join();
    }
    // The input thread released its own registrations on the way out; repeating
    // it here (harmlessly failing for a thread that never ran) means a half
    // started session can never leak a global hotkey.
    unregister_all();
    ll_enabled_.store(false);
    if (ll_hook_ != nullptr) {
        UnhookWindowsHookEx(ll_hook_);
        ll_hook_ = nullptr;
    }
    thread_id_ = 0;
}

void InputThread::set_hotkeys(const std::array<HotkeyChord, kHotkeyCount>& chords,
                              std::vector<HotkeyAction>& out_conflicts) {
    out_conflicts.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chords_ = chords;
    }
    if (!running_.load() || thread_id_ == 0) return;  // nothing to re-register on yet

    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_requested_chords = chords;
        g_requested_conflicts.clear();
        g_reregister_request = true;
    }
    if (PostThreadMessageW(thread_id_, kMsgReRegister, 0, 0) == FALSE) {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_reregister_request = false;
        return;
    }
    std::unique_lock<std::mutex> lock(g_channel_mutex);
    g_channel_cv.wait_for(lock, kHandshakeTimeout, [] { return !g_reregister_request; });
    if (!g_reregister_request) out_conflicts = g_requested_conflicts;
}

bool InputThread::set_low_level_fallback(bool enabled) {
    // Strict compatibility mode (design doc §3.3/§6.5) keeps RegisterHotKey as
    // the only input path; disabling the fallback is still allowed.
    if (enabled && g_strict_compat) return false;
    if (!running_.load() || thread_id_ == 0) return false;

    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_ll_want = enabled;
        g_ll_result = false;
        g_ll_request = true;
    }
    if (PostThreadMessageW(thread_id_, kMsgSetLowLevel, 0, 0) == FALSE) {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_ll_request = false;
        return false;
    }
    std::unique_lock<std::mutex> lock(g_channel_mutex);
    g_channel_cv.wait_for(lock, kHandshakeTimeout, [] { return !g_ll_request; });
    return !g_ll_request && g_ll_result;
}

void InputThread::set_strict_compat(bool strict) {
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_strict_compat = strict;
    }
    // Turning it on has to take the hook down with it, or the flag would say one
    // thing and the keyboard would do another. Turning it off does not put the
    // hook back: whether it is wanted depends on which chords are still in
    // conflict, and the caller re-registers straight afterwards anyway.
    if (strict) set_low_level_fallback(false);
}

void InputThread::set_registration_suspended(bool suspended) {
    if (!running_.load() || thread_id_ == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_suspend_want = suspended;
        g_suspend_request = true;
    }
    if (PostThreadMessageW(thread_id_, kMsgSuspend, 0, 0) == FALSE) {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_suspend_request = false;
        return;
    }
    std::unique_lock<std::mutex> lock(g_channel_mutex);
    g_channel_cv.wait_for(lock, kHandshakeTimeout, [] { return !g_suspend_request; });
}

void InputThread::set_edge_zone(std::optional<RectPx> zone_px, Px band_px, bool enabled) {
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    // Applied by poll_cursor on the next tick; the owner thread must not touch
    // the detector state directly (the header reserves it for this thread).
    g_requested_zone = zone_px;
    g_requested_band = band_px;
    g_requested_edge_enabled = enabled;
    g_edge_request = true;
}

std::string InputThread::registration_report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.empty()) return std::string("all bound hotkeys registered");
    return report_;
}

void InputThread::register_all(const std::array<HotkeyChord, kHotkeyCount>& chords,
                               std::vector<HotkeyAction>& out_conflicts) {
    unregister_all();
    out_conflicts.clear();

    std::vector<HotkeyAction> accepted;
    std::string report;
    std::array<HotkeyChord, kHotkeyCount> fallback{};
    std::array<HotkeyAction, kHotkeyCount> fallback_actions{};
    std::size_t fallback_count = 0;

    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        const HotkeyChord chord = chords[i];
        if (!chord_is_bound(chord)) continue;
        const auto action = static_cast<HotkeyAction>(i);

        // A mouse button never goes to RegisterHotKey, and not because it would
        // refuse: it *accepts* a mouse virtual key and reports success, and the
        // system then never delivers one as a hotkey. Asking would produce a
        // chord that looks registered and does nothing at all, which is the one
        // failure this whole path exists to prevent. The hook is the only place
        // a mouse button can be served, so that is where it goes.
        //
        // The left button is the exception, and it is refused outright: serving
        // it would mean swallowing every click in the session, including the
        // ones that would take the binding back. Nothing in the interface can
        // make such a chord -- a left click on a chord field is what starts the
        // capture -- so this only ever fires for a hand-edited file.
        const bool mouse = is_mouse_button_vk(chord.virtual_key);
        if (chord.virtual_key == VK_LBUTTON) {
            report += describe_chord(chord);
            report += " (";
            report += hotkey_action_name(action);
            report += ") refused: the left button cannot be bound\n";
            continue;
        }
        if (!mouse && RegisterHotKey(nullptr, hotkey_id_for(i), chord.modifiers | MOD_NOREPEAT,
                                     chord.virtual_key) != FALSE) {
            accepted.push_back(action);
            continue;
        }
        const DWORD error = mouse ? 0u : GetLastError();
        out_conflicts.push_back(action);
        // A chord RegisterHotKey refused is exactly what the low-level fallback
        // is for. Only those chords go into the hook table: a chord both paths
        // handled would publish HotkeyPressed twice, and the low-level hook
        // would swallow a key the shell already delivered to us.
        if (fallback_count < fallback.size()) {
            fallback[fallback_count] = chord;
            fallback_actions[fallback_count] = action;
            ++fallback_count;
        }
        report += describe_chord(chord);
        report += " (";
        report += hotkey_action_name(action);
        report += ") ";
        if (mouse) {
            report += "is a mouse button, which only the low-level hook can serve";
        } else if (error == ERROR_HOTKEY_ALREADY_REGISTERED) {
            report += "is already registered by another application";
        } else {
            report += "failed to register, error ";
            report += std::to_string(static_cast<unsigned long>(error));
        }
        report += '\n';
    }

    g_ll_chords = fallback;
    g_ll_actions = fallback_actions;
    g_ll_chord_count = fallback_count;

    std::lock_guard<std::mutex> lock(mutex_);
    registered_ = std::move(accepted);
    report_ = std::move(report);
}

void InputThread::unregister_all() {
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        UnregisterHotKey(nullptr, hotkey_id_for(i));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    registered_.clear();
}

bool InputThread::apply_low_level(bool enabled) {
    if (enabled) {
        if (ll_hook_ == nullptr) {
            // A low-level hook must be installed by the thread that pumps
            // messages, which is this one.
            ll_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputThread::ll_keyboard_proc,
                                         GetModuleHandleW(nullptr), 0);
        }
        if (ll_mouse_hook_ == nullptr && fallback_has_mouse_chord()) {
            // Mouse buttons are only ever reachable this way: RegisterHotKey
            // has no mouse chords at all, so a binding on one of them lives or
            // dies with this hook. It is installed only when a chord actually
            // needs it, because every mouse event in the session -- moves
            // included, hundreds a second -- costs a trip to this thread while
            // it is up, and a chord another application owns on the *keyboard*
            // does not need the mouse hook at all.
            ll_mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, &InputThread::ll_mouse_proc,
                                               GetModuleHandleW(nullptr), 0);
        }
        const bool ready = ll_hook_ != nullptr &&
                           (ll_mouse_hook_ != nullptr || !fallback_has_mouse_chord());
        ll_enabled_.store(ready);
        if (!ready) {
            // Half a hook is worse than none: the chords would report as served
            // while one class of them silently did nothing.
            apply_low_level(false);
        }
        return ready;
    }
    ll_enabled_.store(false);
    if (ll_hook_ != nullptr) {
        UnhookWindowsHookEx(ll_hook_);
        ll_hook_ = nullptr;
    }
    if (ll_mouse_hook_ != nullptr) {
        UnhookWindowsHookEx(ll_mouse_hook_);
        ll_mouse_hook_ = nullptr;
    }
    return true;
}

void InputThread::thread_main() {
    thread_id_ = GetCurrentThreadId();
    ll_owner_ = this;

    // Creating the queue before announcing readiness is what makes the owner
    // thread's PostThreadMessage safe from then on.
    MSG probe{};
    PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_queue_ready = true;
    }
    g_channel_cv.notify_all();

    std::array<HotkeyChord, kHotkeyCount> chords{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chords = chords_;
    }
    std::vector<HotkeyAction> ignored_conflicts;
    register_all(chords, ignored_conflicts);

    g_edge_timer = SetTimer(nullptr, kEdgeTimerId, kEdgePollMs, nullptr);

    MSG msg{};
    while (running_.load(std::memory_order_relaxed)) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) break;  // WM_QUIT

        switch (msg.message) {
            case WM_HOTKEY: {
                const int id = static_cast<int>(msg.wParam);
                const int base = kHotkeyBaseId;
                if (id >= base && id < base + static_cast<int>(kHotkeyCount)) {
                    const auto action = static_cast<HotkeyAction>(id - base);
                    publish_payload(bus_, HotkeyPressed{static_cast<std::uint32_t>(action)});
                }
                break;
            }
            case kMsgReRegister: {
                std::array<HotkeyChord, kHotkeyCount> next{};
                {
                    std::lock_guard<std::mutex> lock(g_channel_mutex);
                    next = g_requested_chords;
                }
                std::vector<HotkeyAction> conflicts;
                // While the chords are released for a rebind, a re-register
                // would put them straight back and swallow the keystroke the
                // user is in the middle of typing. The table is already stored;
                // resuming registers it.
                if (!suspended_) register_all(next, conflicts);
                {
                    std::lock_guard<std::mutex> lock(g_channel_mutex);
                    g_requested_conflicts = std::move(conflicts);
                    g_reregister_request = false;
                }
                g_channel_cv.notify_all();
                break;
            }
            case kMsgSetLowLevel: {
                bool want = false;
                {
                    std::lock_guard<std::mutex> lock(g_channel_mutex);
                    want = g_ll_want;
                }
                const bool result = apply_low_level(want && !g_strict_compat);
                {
                    std::lock_guard<std::mutex> lock(g_channel_mutex);
                    g_ll_result = result;
                    g_ll_request = false;
                }
                g_channel_cv.notify_all();
                break;
            }
            case kMsgSuspend: {
                bool want = false;
                {
                    std::lock_guard<std::mutex> lock(g_channel_mutex);
                    want = g_suspend_want;
                }
                if (want) {
                    // The hook goes first: a chord it owns is swallowed by
                    // returning 1 from the callback, which is precisely what
                    // must not happen while the user is trying to type it.
                    ll_resume_ = ll_enabled_.load();
                    apply_low_level(false);
                    unregister_all();
                    g_ll_chord_count = 0;
                } else {
                    std::array<HotkeyChord, kHotkeyCount> chords{};
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        chords = chords_;
                    }
                    std::vector<HotkeyAction> ignored_conflicts;
                    register_all(chords, ignored_conflicts);
                    if (ll_resume_ && !g_strict_compat) apply_low_level(true);
                    ll_resume_ = false;
                }
                suspended_ = want;
                {
                    std::lock_guard<std::mutex> lock(g_channel_mutex);
                    g_suspend_request = false;
                }
                g_channel_cv.notify_all();
                break;
            }
            case WM_TIMER: {
                if (msg.wParam == static_cast<WPARAM>(g_edge_timer)) poll_cursor();
                break;
            }
            default:
                break;
        }
    }

    if (g_edge_timer != 0) {
        KillTimer(nullptr, g_edge_timer);
        g_edge_timer = 0;
    }
    unregister_all();
    ll_enabled_.store(false);
    if (ll_hook_ != nullptr) {
        UnhookWindowsHookEx(ll_hook_);
        ll_hook_ = nullptr;
    }
    ll_owner_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_queue_ready = false;
    }
}

void InputThread::poll_cursor() {
    bool zone_changed = false;
    bool reported_stay = false;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        if (g_edge_request) {
            zone_changed = true;
            reported_stay = edge_inside_ && edge_fired_;
            g_edge_request = false;
            edge_zone_px_ = g_requested_zone;
            edge_band_px_ = g_requested_band > 0 ? g_requested_band : 1;
            edge_enabled_ = g_requested_edge_enabled && edge_zone_px_.has_value() &&
                            !is_empty(*edge_zone_px_);
            edge_inside_ = false;
            edge_fired_ = false;
            edge_since_ms_ = 0;
        }
    }
    if (!zone_changed && (!edge_enabled_ || !edge_zone_px_.has_value())) return;

    POINT raw{};
    if (GetCursorPos(&raw) == FALSE) return;
    const PointPx cursor{raw.x, raw.y};

    if (zone_changed) {
        // A new zone ends whatever stay was in progress. If the state machine
        // was already told about that stay it has to be told it ended too,
        // otherwise it would keep waiting on a border that no longer exists.
        if (reported_stay) publish_payload(bus_, CursorSampled{cursor, 0});
        return;
    }

    const RectPx zone = *edge_zone_px_;
    const Px band = edge_band_px_;

    const bool inside_zone = cursor.x >= zone.left && cursor.x < zone.right &&
                             cursor.y >= zone.top && cursor.y < zone.bottom;
    const bool on_border = inside_zone &&
                           (cursor.x - zone.left < band || zone.right - cursor.x <= band ||
                            cursor.y - zone.top < band || zone.bottom - cursor.y <= band);
    const std::uint64_t now = GetTickCount64();

    if (on_border) {
        if (!edge_inside_) {
            edge_inside_ = true;
            edge_fired_ = false;
            edge_since_ms_ = now;
            // Nothing is published on the entry tick: the state machine reads a
            // zero dwell as "the cursor left the edge", so a fresh stay must
            // not start by reporting one.
            return;
        }
        // An absolute dwell, not a delta: a dropped event cannot corrupt the
        // state machine's notion of how long the cursor has been parked.
        publish_payload(bus_, CursorSampled{cursor, now - edge_since_ms_});
        edge_fired_ = true;
        return;
    }
    if (edge_inside_) {
        // Leaving the band is reported explicitly; a zero dwell is the only
        // signal that lets the state machine fall back to PassThrough.
        edge_inside_ = false;
        edge_fired_ = false;
        edge_since_ms_ = 0;
        publish_payload(bus_, CursorSampled{cursor, 0});
    }
}

LRESULT CALLBACK InputThread::ll_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    InputThread* self = ll_owner_;
    if (code == HC_ACTION && self != nullptr &&
        self->ll_enabled_.load(std::memory_order_relaxed)) {
        const DWORD event = static_cast<DWORD>(wparam);
        if (event == WM_KEYDOWN || event == WM_SYSKEYDOWN) {
            const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
            if (const std::optional<HotkeyAction> action = match_fallback_chord(info->vkCode)) {
                publish_payload(self->bus_, HotkeyPressed{static_cast<std::uint32_t>(*action)});
                // A nonzero return is what swallows the keystroke: the system
                // stops the chain and no window receives it.
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK InputThread::ll_mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
    InputThread* self = ll_owner_;
    if (code == HC_ACTION && self != nullptr &&
        self->ll_enabled_.load(std::memory_order_relaxed)) {
        std::uint32_t vk = 0;
        switch (wparam) {
            case WM_LBUTTONDOWN: vk = VK_LBUTTON; break;
            case WM_RBUTTONDOWN: vk = VK_RBUTTON; break;
            case WM_MBUTTONDOWN: vk = VK_MBUTTON; break;
            case WM_XBUTTONDOWN: {
                const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
                vk = (HIWORD(info->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                break;
            }
            default:
                // Moves, wheels, ups: nothing here is bindable, and this runs
                // for every one of them.
                break;
        }
        if (vk != 0) {
            if (const std::optional<HotkeyAction> action = match_fallback_chord(vk)) {
                publish_payload(self->bus_, HotkeyPressed{static_cast<std::uint32_t>(*action)});
                // Swallowed, the same rule the keyboard path follows: a chord
                // the program services does not also reach the window under the
                // cursor. Binding the middle button therefore stops middle-drag
                // everywhere, which is what binding it asked for.
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace mag
