// app/app_host.cpp — wiring for the whole application.
//
// Thread layout (design doc §2.3):
//   UI thread      — windows, this file's event pump, which is also the
//                    control thread that owns the domain controllers.
//   input thread   — global hotkeys and cursor edge-dwell sampling.
//   capture thread — one per monitor, inside the capture backends.
//   render thread  — D3D11 + DirectComposition presentation.
//   worker thread  — ConfigStore's atomic writes.
//
// Data crosses those boundaries only as value events on the bus, or as the
// single-slot render snapshot mailbox.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app/app_host.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <memory>
#include <type_traits>
#include <variant>

#include <mmsystem.h>

#include "core/hit_test.h"

namespace mag {

namespace {

AppHost* g_host = nullptr;  // the thread-timer callback carries no user pointer

// Step applied by the keyboard window-resize path (Ctrl+Alt+arrows).
constexpr Px kWindowResizeStepPx = 40;

// Drag bookkeeping shared with the overlay window's callbacks. Held by
// shared_ptr because the callbacks outlive the scope that creates them.
struct DragState {
    PointPx origin_cursor{};
    RectPx origin_rect{};
    SizePx origin_size{};
};

}  // namespace

// ---------------------------------------------------------------------------
// CaptureGroup
// ---------------------------------------------------------------------------

CaptureGroup::CaptureGroup(BoundedEventBus& bus, Win32TopologyService& topology)
    : bus_(bus), topology_(topology) {}

CaptureGroup::~CaptureGroup() {
    stop();
}

void CaptureGroup::build(const std::vector<MonitorInfoPx>& monitors) {
    stop();
    backends_.clear();
    backend_monitors_.clear();
    fallback_ = false;
    note_.clear();
    summary_.clear();

    // Decide the backend family once, with a short-lived probe session. The
    // probe is the only way to learn that DuplicationOutput will be refused --
    // exclusive fullscreen, protected video and some anti-cheat configurations
    // all fail at start() rather than at construction -- and it releases its
    // resources again immediately, so idling before the first toggle costs no
    // GPU memory.
    bool dxgi_ok = true;
    if (!monitors.empty()) {
        auto probe = make_dxgi_backend(bus_);
        try {
            probe->start(monitors.front().id, monitors.front().monitor_rect_px);
            probe->stop();
        } catch (const std::exception& ex) {
            dxgi_ok = false;
            last_error_ = ex.what();
            note_ = std::string("DXGI duplication unavailable on output ") +
                    std::to_string(monitors.front().id) + ": " + ex.what();
        }
    }

    for (const auto& monitor : monitors) {
        std::unique_ptr<ICaptureBackend> backend;
        if (dxgi_ok) {
            backend = make_dxgi_backend(bus_);
        } else {
            // The fallback gets the same probe treatment: an unusable fallback
            // must be reported now, not discovered at the first toggle.
            auto candidate = make_gdi_backend(bus_);
            try {
                candidate->start(monitor.id, monitor.monitor_rect_px);
                candidate->stop();
                backend = std::move(candidate);
                fallback_ = true;
            } catch (const std::exception& ex2) {
                last_error_ = ex2.what();
                note_ += std::string("; GDI fallback also failed: ") + ex2.what();
            }
        }
        if (backend) {
            backends_.push_back(std::move(backend));
            backend_monitors_.push_back(monitor);
        }
    }

    // Sessions stay closed until the magnifier is actually switched on.
    started_ = false;

    if (!backends_.empty()) {
        summary_ = backends_.front()->backend_name();
        if (backends_.size() > 1) summary_ += " x" + std::to_string(backends_.size());
    } else {
        summary_ = "none";
    }

    if (fallback_) {
        ErrorReported err{};
        err.code = 1;
        err.text = "Capture switched to the GDI fallback backend";
        bus_.publish(make_event(err));
    }
}

void CaptureGroup::rebuild(const std::vector<MonitorInfoPx>& monitors) {
    build(monitors);
}

void CaptureGroup::start() noexcept {
    if (started_) return;
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        if (!backends_[i]) continue;
        try {
            backends_[i]->start(backend_monitors_[i].id, backend_monitors_[i].monitor_rect_px);
        } catch (const std::exception& ex) {
            last_error_ = ex.what();
        }
    }
    started_ = true;
}

void CaptureGroup::stop() noexcept {
    if (!started_) return;
    for (auto& backend : backends_) {
        if (backend) backend->stop();
    }
    started_ = false;
}

void CaptureGroup::recover_async() noexcept {
    for (auto& backend : backends_) {
        if (backend) backend->recover_async();
    }
}

std::vector<ICaptureBackend*> CaptureGroup::raw() const {
    std::vector<ICaptureBackend*> out;
    out.reserve(backends_.size());
    for (const auto& backend : backends_) out.push_back(backend.get());
    return out;
}

std::string CaptureGroup::backend_summary() const {
    return summary_.empty() ? std::string("none") : summary_;
}

// ---------------------------------------------------------------------------
// AppHost
// ---------------------------------------------------------------------------

AppHost::AppHost(HINSTANCE instance) : instance_(instance) {
    g_host = this;
}

AppHost::~AppHost() {
    shutdown();
    g_host = nullptr;
}

LRESULT CALLBACK AppHost::pump_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER) {
        if (g_host && !g_host->shutting_down_.load()) g_host->pump_events();
        return 0;
    }
    if (g_host && !g_host->shutting_down_.load()) {
        // The tray icon posts its menu selections here. Without this the menu
        // would appear and do nothing at all.
        unsigned command = 0;
        if (g_host->tray_.handle_message(message, wparam, lparam, command)) {
            if (command != 0) g_host->handle_command(command);
            return 0;
        }
        if (message == WM_DISPLAYCHANGE) {
            // A monitor was added, removed, or had its mode changed, so the
            // topology snapshot and every rectangle derived from it has to be
            // rebuilt; otherwise the selection would address the old layout.
            g_host->notice_ = Notice{Str::DisplayChanged};
            g_host->bus_.publish(make_event(TopologyChanged{0}));
            return 0;
        }
    }
    if (message == WM_NCCREATE) return TRUE;
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

bool AppHost::initialize() {
    // Windows rounds timer waits up to the system tick, which is 15.6 ms by
    // default -- long enough on its own to halve the achievable frame rate.
    ::timeBeginPeriod(1);

    enable_per_monitor_dpi_v2();

    topo_ = topology_.snapshot();
    if (is_empty(topo_.virtual_desktop_px)) {
        last_error_ = "could not determine the virtual desktop extent";
        return false;
    }

    // Set MAG_DIAG=1 to surface the coordinate-space facts in the settings
    // window's notice line. DPI misconfiguration is invisible otherwise: the
    // app still runs, it just addresses every pixel in the wrong space.
    diagnostics_ = ::GetEnvironmentVariableW(L"MAG_DIAG", nullptr, 0) > 0;
    if (diagnostics_) {
        const bool aware =
            ::AreDpiAwarenessContextsEqual(::GetThreadDpiAwarenessContext(),
                                           DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "DIAG aware=%d desktop=(%d,%d) %dx%d monitors=%zu screen=%dx%d",
                      aware ? 1 : 0, topo_.virtual_desktop_px.left, topo_.virtual_desktop_px.top,
                      width_of(topo_.virtual_desktop_px), height_of(topo_.virtual_desktop_px),
                      topo_.monitors.size(), ::GetSystemMetrics(SM_CXSCREEN),
                      ::GetSystemMetrics(SM_CYSCREEN));
        notice_ = Notice{Str::Count, buf};
    }

    load_config_or_defaults();

    // On a first run the shipped region sits in the corner, which is not where
    // anyone wants to look; put it in the middle of the screen instead.
    if (fresh_config_) {
        config_.selection_bounds_px =
            centre_on_primary(width_of(config_.selection_bounds_px),
                              height_of(config_.selection_bounds_px));
        // Every slot starts as the shipped region rather than empty, so
        // recalling one always goes somewhere and the row reads as a set of
        // choices rather than as four dead buttons.
        for (SelectionConfig& slot : config_.selection_slots) {
            slot = SelectionConfig{config_.selection_bounds_px, config_.selection_shape,
                                   config_.selection_corner_radius_px};
        }
        config_.selection_slot = 0;
    }

    selection_ = std::make_unique<SelectionController>(bus_, topo_.virtual_desktop_px);
    selection_->set(SelectionConfig{config_.selection_bounds_px, config_.selection_shape,
                                    config_.selection_corner_radius_px});

    magnifier_ = std::make_unique<MagnificationController>(bus_, topo_.virtual_desktop_px);
    magnifier_->set(MagnificationConfig{config_.factor_q16, config_.output_size_px,
                                        config_.keep_aspect_ratio});
    magnifier_->set_presets(config_.presets);
    if (config_.output_position_auto) {
        // The middle of the screen, not the middle of whichever monitor the
        // selection happens to sit on: that is what a user means by the centre.
        const MonitorInfoPx* mon = topo_.primary();
        if (!mon) mon = topo_.monitor_at(PointPx{config_.selection_bounds_px.left,
                                                 config_.selection_bounds_px.top});
        if (mon) magnifier_->center_on(mon->work_rect_px);
    } else {
        magnifier_->set_output_position(config_.output_position_px);
    }

    captures_ = std::make_unique<CaptureGroup>(bus_, topology_);
    captures_->build(topo_.monitors);

    // --- overlay window ---
    auto drag = std::make_shared<DragState>();
    OverlayWindow::Callbacks overlay_cbs;

    overlay_cbs.on_drag_begin = [this, drag](PointPx cursor) {
        drag->origin_cursor = cursor;
        drag->origin_rect = magnifier_->output_rect();
    };
    overlay_cbs.on_drag_update = [this, drag](PointPx cursor) {
        const Px dx = cursor.x - drag->origin_cursor.x;
        const Px dy = cursor.y - drag->origin_cursor.y;
        magnifier_->set_output_position(
            PointPx{drag->origin_rect.left + dx, drag->origin_rect.top + dy});
        config_.output_position_auto = false;
        apply_geometry_to_window();
        publish_snapshot();
    };
    overlay_cbs.on_drag_end = [this] { save_config(); };

    overlay_cbs.on_resize_begin = [this, drag](ResizeHandle, PointPx) {
        drag->origin_size = magnifier_->current().output_size_px;
        drag->origin_rect = magnifier_->output_rect();
    };
    overlay_cbs.on_resize_update = [this, drag](ResizeHandle handle, PointPx delta) {
        SizePx size = drag->origin_size;
        // Dragging an edge moves that edge and leaves the opposite one where it
        // was, so the left and top handles have to shift the origin as well as
        // change the size. Resizing alone grows the window to the right.
        Px left = drag->origin_rect.left;
        Px top = drag->origin_rect.top;
        if (handle_has(handle, ResizeHandle::Right)) {
            size.width = drag->origin_size.width + delta.x;
        } else if (handle_has(handle, ResizeHandle::Left)) {
            size.width = drag->origin_size.width - delta.x;
            left = drag->origin_rect.left + delta.x;
        }
        if (handle_has(handle, ResizeHandle::Bottom)) {
            size.height = drag->origin_size.height + delta.y;
        } else if (handle_has(handle, ResizeHandle::Top)) {
            size.height = drag->origin_size.height - delta.y;
            top = drag->origin_rect.top + delta.y;
        }
        size.width = std::max<Px>(size.width, kMinOutputEdgePx);
        size.height = std::max<Px>(size.height, kMinOutputEdgePx);
        try {
            magnifier_->resize_output(size);
        } catch (const std::out_of_range&) {
            return;
        }
        config_.output_size_px = magnifier_->current().output_size_px;
        if (handle_has(handle, ResizeHandle::Left) || handle_has(handle, ResizeHandle::Top)) {
            config_.output_position_auto = false;
            magnifier_->set_output_position(PointPx{left, top});
        }
        apply_geometry_to_window();
        publish_snapshot();
    };
    overlay_cbs.on_resize_end = [this] { save_config(); };

    overlay_cbs.on_client_size_changed = [this](SizePx size) {
        if (render_) render_->notify_client_size(size);
    };
    overlay_cbs.on_close_requested = [this] { set_pass_through(true); };
    overlay_cbs.on_interaction = [this] {
        if (fsm_) fsm_->on_window_interaction();
        publish_snapshot();
    };
    overlay_cbs.on_position_changed = [this](PointPx) { apply_geometry_to_window(); };

    try {
        overlay_.create(instance_, std::move(overlay_cbs));
    } catch (const std::exception& ex) {
        last_error_ = std::string("overlay window creation failed: ") + ex.what();
        return false;
    }
    // Always apply the configured value rather than only applying the "on"
    // case: a window keeps whatever affinity it was last given, so leaving the
    // off case to a default makes the setting look like it does nothing.
    exclude_from_capture_active_ = overlay_.exclude_from_capture(config_.exclude_self_from_capture);
    if (config_.exclude_self_from_capture && !exclude_from_capture_active_) {
        notice_ = Notice{Str::ExcludeRefused};
    }

    // --- region picker ---
    SelectionOverlay::Callbacks picker_cbs;
    picker_cbs.on_committed = [this](const SelectionConfig& sel) { commit_selection(sel); };
    picker_cbs.on_preview = [](const SelectionConfig&) {};
    picker_cbs.on_shape_changed = [this](SelectionShape shape) { config_.selection_shape = shape; };
    picker_cbs.on_cancelled = [] {};
    picker_.create(instance_, topo_.virtual_desktop_px, std::move(picker_cbs));

    // --- state machine ---
    fsm_ = std::make_unique<InteractionStateMachine>(bus_, overlay_, *captures_);

    // --- input thread ---
    input_ = std::make_unique<InputThread>(bus_);
    if (!input_->start(config_.hotkeys, config_.strict_compat_mode)) {
        last_error_ = "the input thread could not be started";
        return false;
    }
    reregister_hotkeys();

    // --- render thread ---
    RenderService::Config render_cfg;
    render_cfg.hwnd = overlay_.hwnd();
    render_cfg.virtual_desktop_px = topo_.virtual_desktop_px;
    render_cfg.captures = captures_->raw();
    render_cfg.target_fps = config_.target_fps;
    render_cfg.filter = config_.scale_filter;
    render_cfg.show_border = config_.show_border;
    render_cfg.exclude_from_capture = config_.exclude_self_from_capture;
    render_ = std::make_unique<RenderService>(render_cfg, mailbox_);

    render_thread_ = std::jthread([this](std::stop_token token) {
        try {
            render_->run(token);
        } catch (const std::exception& ex) {
            // A render failure must not take the process down: the window stays
            // blank and the settings window explains why. Reported through the
            // bus because this runs on the render thread and the message is
            // consumed by the UI thread.
            ErrorReported report{};
            report.code = 2;
            report.text = std::string("renderer unavailable: ") + ex.what();
            bus_.publish(make_event(std::move(report)));
        }
    });

    // --- settings window ---
    ControlWindow::Callbacks ui;
    ui.on_pick_region = [this] { open_picker(); };
    ui.on_toggle_magnifier = [this] { toggle_magnifier(); };
    ui.on_toggle_pass_through = [this] {
        set_pass_through(fsm_->state() != InteractionState::PassThrough);
    };
    ui.on_shape_changed = [this](SelectionShape shape) { set_shape(shape); };
    ui.on_selection_slot = [this](int index) { recall_selection_slot(index); };
    ui.on_save_selection = [this] { save_selection_slot(); };
    ui.on_factor_changed = [this](Q16 factor) { set_factor(factor); };
    ui.on_presets_changed = [this](const std::array<Q16, kPresetCount>& presets) {
        config_.presets = presets;
        // The controller holds its own copy, which is what the preset hotkeys
        // read; without this the buttons and the chords would disagree.
        if (magnifier_) magnifier_->set_presets(presets);
        save_config();
        ui_dirty_ = true;
    };
    ui.on_output_size_changed = [this](SizePx size) { set_output_size(size); };
    ui.on_keep_aspect_changed = [this](bool keep) {
        config_.keep_aspect_ratio = keep;
        if (magnifier_) magnifier_->set_keep_aspect_ratio(keep);
        publish_snapshot();
        save_config();
    };
    ui.on_output_position_changed = [this](PointPx p) {
        config_.output_position_auto = false;
        if (magnifier_) magnifier_->set_output_position(p);
        apply_geometry_to_window();
        publish_snapshot();
    };
    ui.on_chord_capture = [this](bool capturing) {
        // RegisterHotKey swallows the combinations this program owns, so while
        // the settings window is waiting for a chord they are all released:
        // otherwise pressing Ctrl+Alt+M to rebind something else would toggle
        // the magnifier and the field would never see the key.
        if (input_) input_->set_registration_suspended(capturing);
    };
    ui.on_hotkeys_changed = [this](const std::array<HotkeyChord, kHotkeyCount>& chords) {
        config_.hotkeys = chords;
        reregister_hotkeys();
        save_config();
        ui_dirty_ = true;
    };
    ui.on_strict_compat_changed = [this](bool strict) {
        config_.strict_compat_mode = strict;
        if (input_) input_->set_strict_compat(strict);
        // The fallback may be wanted or refused differently now, so the chords
        // are settled again rather than only torn down when it is turned on.
        reregister_hotkeys();
        save_config();
    };
    ui.on_edge_dwell_changed = [this](Px band, int ms, bool enabled) {
        config_.edge_dwell_band_px = band;
        config_.edge_dwell_ms = ms;
        config_.edge_dwell_enabled = enabled;
        apply_geometry_to_window();
        save_config();
    };
    ui.on_filter_changed = [this](ScaleFilter filter) {
        config_.scale_filter = filter;
        if (render_) render_->set_filter(filter);
        save_config();
    };
    ui.on_restore_defaults = [this] { restore_defaults(); };
    ui.on_save_config = [this] {
        save_config();
        if (magnifier_) magnifier_->set_presets(config_.presets);
    };
    ui.on_quit = [this] { quit(); };
    ui.on_recover_capture = [this] {
        if (captures_) captures_->recover_async();
    };
    ui.on_exclude_from_capture_changed = [this](bool enable) {
        config_.exclude_self_from_capture = enable;
        exclude_from_capture_active_ = overlay_.exclude_from_capture(enable);
        save_config();
    };
    ui.on_show_border_changed = [this](bool show) {
        config_.show_border = show;
        save_config();
    };
    ui.on_language_changed = [this](Language lang) {
        // The settings window has already retranslated itself; the rest of the
        // process -- the tray, the status line -- follows from here.
        set_language(lang);
        config_.language = lang;
        tray_.set_tooltip(tr(Str::TrayTooltip));
        refresh_status();
        save_config();
        ui_dirty_ = true;
    };

    try {
        control_.create(instance_, std::move(ui), [] {});
    } catch (const std::exception& ex) {
        last_error_ = std::string("settings window creation failed: ") + ex.what();
        return false;
    }

    // --- host window: control pump, tray owner and display-change receiver ---
    // A real but never-shown top-level window, not a message-only one: WM_TIMER
    // and the tray callback would reach either, but WM_DISPLAYCHANGE is
    // broadcast only to top-level windows.
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &AppHost::pump_wnd_proc;
        wc.hInstance = instance_;
        wc.lpszClassName = L"MagHostPumpWindow";
        ::RegisterClassExW(&wc);  // a second registration failure is harmless
        pump_hwnd_ = ::CreateWindowExW(0, L"MagHostPumpWindow", L"", WS_POPUP, 0, 0, 0, 0,
                                       nullptr, nullptr, instance_, nullptr);
        if (pump_hwnd_) {
            pump_timer_ = ::SetTimer(pump_hwnd_, 1, 30, nullptr);
        }
        tray_.create(pump_hwnd_, instance_, tr(Str::TrayTooltip));
        tray_.set_visible(true);
    }

    publish_snapshot();
    refresh_status();
    push_ui_state();
    control_.show();

    // Honour the persisted "start in pass-through" preference. The window is
    // shown first so there is a real rectangle for the hit test to apply to.
    if (config_.start_in_pass_through) {
        toggle_magnifier();
        set_pass_through(true);
    }
    return true;
}

void AppHost::load_config_or_defaults() {
    config_ = AppConfig::defaults();
    std::ifstream probe(ConfigStore::default_path());
    fresh_config_ = !probe.good();
    AppConfig loaded = config_;
    store_.load_now(ConfigStore::default_path(), loaded, config_);
    config_ = loaded;
    if (!validate(config_, topo_.virtual_desktop_px)) {
        config_ = AppConfig::defaults();
        validate(config_, topo_.virtual_desktop_px);
        notice_ = Notice{Str::SettingsRestored};
    }
    // The stored language has to be in force before any window is created, or
    // the UI would come up in the wrong one.
    set_language(config_.language);
}

int AppHost::run() {
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void AppHost::shutdown() noexcept {
    if (shutting_down_.exchange(true)) return;

    if (pump_hwnd_) {
        if (pump_timer_) ::KillTimer(pump_hwnd_, pump_timer_);
        ::DestroyWindow(pump_hwnd_);
        pump_hwnd_ = nullptr;
        pump_timer_ = 0;
    }

    save_config();

    if (input_) {
        input_->stop();
        input_.reset();
    }

    if (render_) render_->request_stop();
    if (render_thread_.joinable()) render_thread_.join();
    render_thread_ = std::jthread{};

    if (captures_) captures_->stop();
    render_.reset();
    captures_.reset();

    overlay_.destroy();
    picker_.destroy();
    tray_.destroy();
    control_.destroy();

    store_.flush();
    ::timeEndPeriod(1);
}

void AppHost::quit() {
    if (input_) input_->stop();
    ::PostQuitMessage(0);
}

// --- event pumping ---------------------------------------------------------

void AppHost::pump_events() {
    // The handler runs on this thread, inside drain(), so handle_event may
    // touch domain state directly without further synchronisation.
    bus_.set_handler(
        [](void* ctx, const AppEvent& event) { static_cast<AppHost*>(ctx)->handle_event(event); },
        this);
    bus_.drain(256);
    bus_.set_handler(nullptr, nullptr);

    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    ::QueryPerformanceCounter(&counter);
    ::QueryPerformanceFrequency(&frequency);
    const std::uint64_t now_qpc = static_cast<std::uint64_t>(counter.QuadPart);
    const std::uint64_t refresh_period = static_cast<std::uint64_t>(frequency.QuadPart / 2);
    // Nothing below is worth doing for a window the user cannot see, and the
    // status line changes on every refresh, so an invisible window would be
    // reformatting and re-setting text several times a second for nothing.
    const bool control_visible =
        control_.hwnd() != nullptr && ::IsWindowVisible(control_.hwnd()) != FALSE;
    if (!control_visible) {
        last_ui_refresh_qpc_ = now_qpc;
        ui_dirty_ = true;   // push a fresh state when it is shown again
        return;
    }

    if (last_ui_refresh_qpc_ == 0 || now_qpc - last_ui_refresh_qpc_ >= refresh_period) {
        last_ui_refresh_qpc_ = now_qpc;
        refresh_status();
        ui_dirty_ = true;

        // Once the magnifier is off and the render thread has had time to drop
        // its device, hand the pages back so the advertised idle footprint is
        // what the user actually sees in Task Manager.
        if (ever_started_ && fsm_ && fsm_->state() == InteractionState::Off &&
            !working_set_trimmed_) {
            working_set_trimmed_ = true;
            ::SetProcessWorkingSetSize(::GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                       static_cast<SIZE_T>(-1));
        }
    }

    if (ui_dirty_.exchange(false)) {
        push_ui_state();
    }
}

void AppHost::handle_event(const AppEvent& event) {
    std::visit(
        [this](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, HotkeyPressed>) {
                handle_hotkey(static_cast<HotkeyAction>(payload.id));
            } else if constexpr (std::is_same_v<T, SnapshotChanged>) {
                // A single controller only knows its own slice of the state: the
                // magnification controller cannot see the selection, and vice
                // versa. Rebuild the snapshot from all three owners before it
                // reaches the renderer, otherwise the renderer would be handed a
                // snapshot with a default-constructed (empty) selection.
                (void)payload;
                mirror_config_from_controllers();
                publish_snapshot();
                apply_geometry_to_window();
                refresh_status();
                ui_dirty_ = true;
            } else if constexpr (std::is_same_v<T, SelectionChanged>) {
                mirror_config_from_controllers();
                publish_snapshot();
                refresh_status();
                ui_dirty_ = true;
            } else if constexpr (std::is_same_v<T, StateChanged>) {
                if (payload.value != InteractionState::Off) {
                    working_set_trimmed_ = false;
                    ever_started_ = true;
                }
                overlay_.apply_interaction_state(payload.value);
                mirror_config_from_controllers();
                publish_snapshot();
                refresh_status();
                ui_dirty_ = true;
            } else if constexpr (std::is_same_v<T, CaptureLost>) {
                handle_capture_lost(payload.output_id);
            } else if constexpr (std::is_same_v<T, CaptureRecovered>) {
                handle_capture_recovered(payload.output_id);
            } else if constexpr (std::is_same_v<T, TopologyChanged>) {
                on_topology_changed();
            } else if constexpr (std::is_same_v<T, CursorSampled>) {
                // Forwarded while armed as well as while passing through: the
                // armed state has to fall back to pass-through when the cursor
                // leaves the band. The zone itself is configured when the state
                // changes and never here -- re-arming on every sample restarts
                // the detector's dwell clock, so the threshold is never reached.
                const InteractionState st = fsm_ ? fsm_->state() : InteractionState::Off;
                if (st == InteractionState::PassThrough || st == InteractionState::EdgeArmed) {
                    const int threshold =
                        config_.edge_dwell_enabled ? config_.edge_dwell_ms : (1 << 30);
                    fsm_->on_edge_dwell(payload.dwell_ms, threshold);
                }
            } else if constexpr (std::is_same_v<T, CommandIssued>) {
                handle_command(payload.id);
            } else if constexpr (std::is_same_v<T, ErrorReported>) {
                // Code 2 is the renderer, whose failure belongs in its own
                // field so a later capture message cannot bury it.
                if (payload.code == 2) {
                    render_error_ = payload.text;
                } else {
                    notice_ = Notice{Str::Count,
                                     payload.text.empty() ? "capture error"
                                                          : payload.text};
                }
                refresh_status();
                ui_dirty_ = true;
            } else if constexpr (std::is_same_v<T, RenderFault>) {
                notice_ = Notice{Str::RendererUnavailable};
                ui_dirty_ = true;
            } else if constexpr (std::is_same_v<T, QuitRequested>) {
                quit();
            }
        },
        event.payload);
}

void AppHost::handle_hotkey(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::ToggleMagnifier:
            toggle_magnifier();
            break;
        case HotkeyAction::TogglePassThrough:
            set_pass_through(fsm_->state() != InteractionState::PassThrough);
            break;
        case HotkeyAction::CycleShape:
            cycle_shape();
            break;
        case HotkeyAction::ZoomIn:
            step_factor(+1);
            break;
        case HotkeyAction::ZoomOut:
            step_factor(-1);
            break;
        case HotkeyAction::Preset1:
        case HotkeyAction::Preset2:
        case HotkeyAction::Preset3:
        case HotkeyAction::Preset4: {
            const auto index = static_cast<std::size_t>(action) -
                               static_cast<std::size_t>(HotkeyAction::Preset1);
            try {
                config_.factor_q16 = magnifier_->select_preset(index);
                if (selection_) magnifier_->fit_output_to_selection(selection_->current());
                config_.output_size_px = magnifier_->current().output_size_px;
                apply_geometry_to_window();
                publish_snapshot();
                // Persisted like any other zoom change: a preset the user chose
                // should still be in force next time.
                save_config();
                ui_dirty_ = true;
            } catch (const std::out_of_range&) {
            }
            break;
        }
        case HotkeyAction::GrowWidth:
            magnifier_->step_output_size(kWindowResizeStepPx, 0);
            break;
        case HotkeyAction::ShrinkWidth:
            magnifier_->step_output_size(-kWindowResizeStepPx, 0);
            break;
        case HotkeyAction::GrowHeight:
            magnifier_->step_output_size(0, kWindowResizeStepPx);
            break;
        case HotkeyAction::ShrinkHeight:
            magnifier_->step_output_size(0, -kWindowResizeStepPx);
            break;
        case HotkeyAction::ResetSelection:
            reset_selection();
            break;
        case HotkeyAction::CenterOutput:
            centre_output_window();
            break;
        case HotkeyAction::Quit:
            quit();
            break;
        case HotkeyAction::Count:
            break;
    }
}

void AppHost::handle_command(unsigned command) {
    switch (command) {
        case TrayIcon::CmdToggle: toggle_magnifier(); break;
        case TrayIcon::CmdPickRegion: open_picker(); break;
        case TrayIcon::CmdTogglePassThrough:
            set_pass_through(fsm_->state() != InteractionState::PassThrough);
            break;
        case TrayIcon::CmdCycleShape: cycle_shape(); break;
        case TrayIcon::CmdSettings:
            control_.show();
            control_.bring_to_front();
            break;
        case TrayIcon::CmdReloadConfig:
            load_config_or_defaults();
            publish_snapshot();
            refresh_status();
            ui_dirty_ = true;
            break;
        case TrayIcon::CmdQuit: quit(); break;
        default: break;
    }
}

// --- operations ------------------------------------------------------------

void AppHost::toggle_magnifier() {
    if (!fsm_) return;
    if (fsm_->state() == InteractionState::Off) {
        fsm_->request_toggle();
    } else {
        fsm_->request_close();
    }
    if (input_) {
        const bool live = fsm_->state() != InteractionState::Off;
        input_->set_edge_zone(live ? std::optional<RectPx>(overlay_.window_rect_px())
                                   : std::nullopt,
                              config_.edge_dwell_band_px, live && config_.edge_dwell_enabled);
    }
    publish_snapshot();
    refresh_status();
    ui_dirty_ = true;
}

void AppHost::set_pass_through(bool enable) {
    if (!fsm_) return;
    if (fsm_->state() == InteractionState::Off) {
        if (!enable) return;
        fsm_->request_toggle();
    }
    fsm_->request_pass_through(enable);
    if (input_) {
        input_->set_edge_zone(overlay_.window_rect_px(), config_.edge_dwell_band_px,
                              config_.edge_dwell_enabled && enable);
    }
    publish_snapshot();
    refresh_status();
    ui_dirty_ = true;
}

void AppHost::set_shape(SelectionShape shape) {
    if (!selection_) return;
    try {
        selection_->set_shape(shape);
    } catch (const std::invalid_argument&) {
        return;
    }
    config_.selection_shape = shape;
    publish_snapshot();
    save_config();
    ui_dirty_ = true;
}

void AppHost::cycle_shape() {
    if (!selection_) return;
    const auto next = (static_cast<int>(selection_->current().shape) + 1) % 4;
    set_shape(static_cast<SelectionShape>(next));
}

void AppHost::set_factor(Q16 factor) {
    if (!magnifier_) return;
    try {
        magnifier_->set_factor(factor);
    } catch (const std::out_of_range&) {
        return;
    }
    // The factor is what the user asked for, so the window is refitted to it:
    // without this the visible scale would be whatever the last window resize
    // implied and the zoom hotkeys would change the reported number only.
    if (selection_) magnifier_->fit_output_to_selection(selection_->current());
    config_.factor_q16 = magnifier_->current().factor_q16;
    config_.output_size_px = magnifier_->current().output_size_px;
    apply_geometry_to_window();
    publish_snapshot();
    save_config();
    ui_dirty_ = true;
}

void AppHost::step_factor(int steps) {
    if (!magnifier_) return;
    magnifier_->step_factor(steps);
    if (selection_) magnifier_->fit_output_to_selection(selection_->current());
    config_.factor_q16 = magnifier_->current().factor_q16;
    config_.output_size_px = magnifier_->current().output_size_px;
    apply_geometry_to_window();
    publish_snapshot();
    save_config();
    ui_dirty_ = true;
}

void AppHost::set_output_size(SizePx size) {
    if (!magnifier_) return;
    if (size.width <= 0 || size.height <= 0) {
        // The settings window's "fit to source" sends an empty size, which is
        // the one window shape that shows the whole region and nothing else.
        // It used to be sent straight to resize_output(), whose range check
        // rejected it -- so the button did nothing at all, in either direction.
        if (selection_) magnifier_->fit_output_to_selection(selection_->current());
    } else {
        try {
            magnifier_->resize_output(size);
        } catch (const std::out_of_range&) {
            return;
        }
    }
    config_.output_size_px = magnifier_->current().output_size_px;
    apply_geometry_to_window();
    publish_snapshot();
    save_config();
    ui_dirty_ = true;
}

void AppHost::commit_selection(const SelectionConfig& sel) {
    if (!selection_ || !magnifier_) return;
    try {
        selection_->set(sel);
    } catch (const std::invalid_argument&) {
        return;
    }
    config_.selection_bounds_px = sel.bounds_px;
    config_.selection_shape = sel.shape;
    config_.selection_corner_radius_px = sel.corner_radius_px;

    // The shipped relationship is "window size follows source size", so a new
    // region re-fits the window.
    magnifier_->fit_output_to_selection(sel);
    config_.output_size_px = magnifier_->current().output_size_px;
    if (config_.output_position_auto) {
        const MonitorInfoPx* mon = topo_.primary();
        if (!mon) mon = topo_.monitor_at(PointPx{sel.bounds_px.left, sel.bounds_px.top});
        if (mon) magnifier_->center_on(mon->work_rect_px);
    }
    apply_geometry_to_window();
    publish_snapshot();
    save_config();
    ui_dirty_ = true;
}

RectPx AppHost::centre_on_primary(Px width, Px height) const {
    const MonitorInfoPx* mon = topo_.primary();
    const RectPx work = mon ? mon->work_rect_px : topo_.virtual_desktop_px;
    const Px w = std::min(width, width_of(work));
    const Px h = std::min(height, height_of(work));
    const Px left = work.left + (width_of(work) - w) / 2;
    const Px top = work.top + (height_of(work) - h) / 2;
    return RectPx{left, top, left + w, top + h};
}

void AppHost::restore_defaults() {
    // Every setting returns to its shipped value. The two placements the user
    // has no way to express in the file -- where the region and the window sit
    // -- go back to the middle of the screen, which is where a first run puts
    // them.
    config_ = AppConfig::defaults();
    validate(config_, topo_.virtual_desktop_px);
    set_language(config_.language);

    if (selection_) {
        config_.selection_bounds_px = centre_on_primary(width_of(config_.selection_bounds_px),
                                                        height_of(config_.selection_bounds_px));
        // The kept regions go back to the shipped region as well, and to the
        // middle of the screen: validate() can only leave an empty slot where
        // the shipped rectangle is, which is the corner, and a slot that
        // recalls a region in the corner is worse than no slot at all.
        for (SelectionConfig& slot : config_.selection_slots) {
            slot = SelectionConfig{config_.selection_bounds_px, config_.selection_shape,
                                   config_.selection_corner_radius_px};
        }
        config_.selection_slot = 0;
        selection_->set(SelectionConfig{config_.selection_bounds_px, config_.selection_shape,
                                        config_.selection_corner_radius_px});
    }
    if (magnifier_) {
        magnifier_->set(MagnificationConfig{config_.factor_q16, config_.output_size_px,
                                            config_.keep_aspect_ratio});
        magnifier_->set_presets(config_.presets);
        magnifier_->center_on(topo_.primary() ? topo_.primary()->work_rect_px
                                              : topo_.virtual_desktop_px);
    }
    reregister_hotkeys();
    if (render_) render_->set_filter(config_.scale_filter);
    overlay_.exclude_from_capture(config_.exclude_self_from_capture);
    // Keep the renderer's copy in step as well: it is what re-asserts the
    // affinity after it rebuilds the composition target.
    if (render_) render_->set_exclude_from_capture(config_.exclude_self_from_capture);
    if (control_.hwnd()) control_.retranslate();
    tray_.set_tooltip(tr(Str::TrayTooltip));

    apply_geometry_to_window();
    publish_snapshot();
    refresh_status();
    save_config();
    ui_dirty_ = true;
}

void AppHost::reregister_hotkeys() {
    if (!input_) return;
    std::vector<HotkeyAction> conflicts;
    input_->set_hotkeys(config_.hotkeys, conflicts);
    // A mouse button always lands here: RegisterHotKey has no mouse chords at
    // all, so it is not a conflict with anything, and the notice has to say
    // what is really happening rather than blame another application.
    bool mouse = false;
    for (const HotkeyAction action : conflicts) {
        if (chord_is_mouse_button(config_.hotkeys[static_cast<std::size_t>(action)])) {
            mouse = true;
        }
    }
    if (conflicts.empty()) {
        input_->set_low_level_fallback(false);
        notice_ = Notice{};
        return;
    }
    // A chord another application already owns is what the low-level fallback
    // exists for. Without this the hotkey silently does nothing, which is the
    // first thing a new user meets and the likeliest reason to conclude the
    // program does not work at all. Strict compatibility mode is the one case
    // that refuses the hook, and that is the user asking for it.
    if (input_->set_low_level_fallback(true)) {
        notice_ = mouse ? Notice{Str::HotkeyMouseHooked} : Notice{Str::HotkeyTakenOver};
        return;
    }
    if (mouse) {
        notice_ = Notice{Str::HotkeyMouseRefused};
        return;
    }
    notice_ = Notice{Str::HotkeyConflictCount, std::to_string(conflicts.size()), true};
}

void AppHost::recall_selection_slot(int index) {
    if (!selection_ || index < 0 || index >= kSelectionSlotCount) return;
    commit_selection(config_.selection_slots[index]);
    // commit_selection treats the region as free-hand, which is right for the
    // picker and wrong here: this one came out of a slot and should stay
    // associated with it, so that saving writes back over the same slot.
    config_.selection_slot = index;
    save_config();
    ui_dirty_ = true;
}

void AppHost::save_selection_slot() {
    if (!selection_) return;
    const int index = config_.selection_slot >= 0 ? config_.selection_slot : 0;
    config_.selection_slots[index] = selection_->current();
    config_.selection_slot = index;
    save_config();
    ui_dirty_ = true;
}

void AppHost::reset_selection() {
    if (!selection_ || !magnifier_) return;
    const RectPx desktop = topo_.virtual_desktop_px;
    const Px w = std::max<Px>(kMinOutputEdgePx, width_of(desktop) / 5);
    const Px h = std::max<Px>(kMinOutputEdgePx, height_of(desktop) / 5);
    RectPx rect{desktop.left + (width_of(desktop) - w) / 2,
                desktop.top + (height_of(desktop) - h) / 2, 0, 0};
    rect.right = rect.left + w;
    rect.bottom = rect.top + h;
    commit_selection(SelectionConfig{
        rect, config_.selection_shape,
        clamp_corner_radius(config_.selection_corner_radius_px, rect)});
}

void AppHost::centre_output_window() {
    if (!magnifier_) return;
    // The window is clamped into the virtual desktop on every move, so a window
    // wider than the screen would otherwise sit against an edge rather than in
    // the middle. Centring on the monitor's work rectangle is what the window
    // started out doing, which is the thing the user is asking to get back.
    const MonitorInfoPx* primary = topo_.primary();
    magnifier_->center_on(primary ? primary->work_rect_px : topo_.virtual_desktop_px);
    config_.output_position_px = magnifier_->output_position();
    apply_geometry_to_window();
    publish_snapshot();
    save_config();
    ui_dirty_ = true;
}

void AppHost::open_picker() {
    if (picker_.active() || !selection_) return;
    picker_.set_virtual_desktop(topo_.virtual_desktop_px);
    picker_.open(selection_->current());
}

void AppHost::on_topology_changed() {
    topology_.invalidate();
    topo_ = topology_.snapshot();
    if (is_empty(topo_.virtual_desktop_px)) return;

    if (selection_) selection_->set_virtual_bounds(topo_.virtual_desktop_px);
    if (magnifier_) magnifier_->set_virtual_bounds(topo_.virtual_desktop_px);
    picker_.set_virtual_desktop(topo_.virtual_desktop_px);

    // Rebuild capture for the new monitor set. The render service holds raw
    // backend pointers, so it is rebuilt after the capture group is replaced;
    // the domain configuration survives untouched, as the design requires.
    const bool was_running = captures_ && captures_->running();
    if (captures_) captures_->rebuild(topo_.monitors);
    if (was_running && captures_) captures_->start();

    if (render_) {
        const int fps = config_.target_fps;
        render_->request_stop();
    }
    if (render_thread_.joinable()) render_thread_.join();

    RenderService::Config cfg;
    cfg.hwnd = overlay_.hwnd();
    cfg.virtual_desktop_px = topo_.virtual_desktop_px;
    cfg.captures = captures_ ? captures_->raw() : std::vector<ICaptureBackend*>{};
    cfg.target_fps = config_.target_fps;
    cfg.filter = config_.scale_filter;
    cfg.show_border = config_.show_border;
    cfg.exclude_from_capture = config_.exclude_self_from_capture;
    render_ = std::make_unique<RenderService>(cfg, mailbox_);
    render_thread_ = std::jthread([this](std::stop_token token) {
        try {
            render_->run(token);
        } catch (const std::exception& ex) {
            ErrorReported report{};
            report.code = 2;
            report.text = std::string("renderer unavailable: ") + ex.what();
            bus_.publish(make_event(std::move(report)));
        }
    });

    apply_geometry_to_window();
    publish_snapshot();
    refresh_status();
    ui_dirty_ = true;
}

void AppHost::handle_capture_lost(std::uint32_t output_id) {
    (void)output_id;
    suspended_ = true;
    if (fsm_) fsm_->on_capture_lost();
    overlay_.set_content_stale(true);
    notice_ = Notice{Str::CaptureLost};
    refresh_status();
    ui_dirty_ = true;
}

void AppHost::handle_capture_recovered(std::uint32_t output_id) {
    (void)output_id;
    if (!suspended_) return;
    suspended_ = false;
    overlay_.set_content_stale(false);
    if (fsm_) fsm_->on_capture_recovered();
    notice_ = Notice{};
    publish_snapshot();
    refresh_status();
    ui_dirty_ = true;
}

// --- publishing ------------------------------------------------------------

void AppHost::publish_snapshot() {
    if (!render_ || !selection_ || !magnifier_ || !fsm_) return;
    RenderSnapshot snapshot{};
    snapshot.selection = selection_->current();
    snapshot.magnification = magnifier_->current();
    snapshot.interaction_state = fsm_->state();
    snapshot.revision = mailbox_.sequence() + 1;
    render_->submit_snapshot(snapshot);
}

void AppHost::apply_geometry_to_window() {
    if (!magnifier_) return;
    overlay_.apply_geometry(magnifier_->output_rect());
    config_.output_position_px = magnifier_->output_position();
    if (input_) {
        input_->set_edge_zone(overlay_.window_rect_px(), config_.edge_dwell_band_px,
                              config_.edge_dwell_enabled);
    }
}

void AppHost::refresh_status() {
    // The settings window composes the readable line -- state, rectangles, frame
    // rate, backend -- from its own translated strings. What it cannot know is
    // why the backend changed or that the renderer gave up, so this carries
    // exactly those notices and nothing else.
    status_text_.clear();
    if (!render_error_.empty()) status_text_ = render_error_;
    if (!notice_.empty()) {
        if (!status_text_.empty()) status_text_ += "   ";
        // Composed here rather than when the event arrived, so the line is in
        // whatever language is in force now.
        if (notice_.detail_first) status_text_ += notice_.detail;
        if (notice_.id != Str::Count) status_text_ += tr_utf8(notice_.id);
        if (!notice_.detail_first) status_text_ += notice_.detail;
    }
    // The exact HRESULT is what turns "capture is broken" into something
    // actionable, so the backend's own words belong on screen too.
    if (captures_ && captures_->using_fallback() && !captures_->last_error().empty()) {
        if (!status_text_.empty()) status_text_ += "   ";
        status_text_ += "DXGI: " + captures_->last_error();
    }

    // The frame counters stay available behind MAG_DIAG=1: "0 fps" alone cannot
    // distinguish a stopped render loop from a desktop that is not changing.
    if (diagnostics_) {
        // The layout geometry, which is otherwise invisible when a rectangle
        // lands somewhere unexpected.
        const MonitorInfoPx* mon = topo_.primary();
        char geo[192];
        std::snprintf(geo, sizeof(geo), "   [desktop=(%d,%d) %dx%d primary_work=(%d,%d) %dx%d]",
                      topo_.virtual_desktop_px.left, topo_.virtual_desktop_px.top,
                      width_of(topo_.virtual_desktop_px), height_of(topo_.virtual_desktop_px),
                      mon ? mon->work_rect_px.left : 0, mon ? mon->work_rect_px.top : 0,
                      mon ? width_of(mon->work_rect_px) : 0, mon ? height_of(mon->work_rect_px) : 0);
        status_text_ += geo;
        const RenderStats stats = render_ ? render_->stats() : RenderStats{};
        char buf[288];
        std::snprintf(buf, sizeof(buf),
                      "   [frames=%llu skip=%llu capTimeout=%llu resets=%llu stage=%u "
                      "ticks=%llu sub=%llu con=%llu conState=%u]",
                      static_cast<unsigned long long>(stats.frames_presented),
                      static_cast<unsigned long long>(stats.frames_skipped),
                      static_cast<unsigned long long>(stats.capture_timeouts),
                      static_cast<unsigned long long>(stats.device_resets), stats.stage,
                      static_cast<unsigned long long>(stats.loop_ticks),
                      static_cast<unsigned long long>(mailbox_.sequence()),
                      static_cast<unsigned long long>(stats.consumed_sequence),
                      stats.consumed_state);
        status_text_ += buf;
    }
}

void AppHost::push_ui_state() {
    if (!control_.hwnd() || !selection_ || !magnifier_ || !fsm_) return;
    const RenderStats stats = render_ ? render_->stats() : RenderStats{};
    control_.sync(config_, fsm_->state(), stats, selection_->current().bounds_px,
                  magnifier_->output_rect(), status_text_,
                  captures_ ? captures_->backend_summary() : "none",
                  captures_ && captures_->using_fallback());
}

void AppHost::mirror_config_from_controllers() {
    if (selection_) {
        config_.selection_bounds_px = selection_->current().bounds_px;
        config_.selection_shape = selection_->current().shape;
        config_.selection_corner_radius_px = selection_->current().corner_radius_px;
    }
    if (magnifier_) {
        config_.factor_q16 = magnifier_->current().factor_q16;
        config_.output_size_px = magnifier_->current().output_size_px;
        config_.keep_aspect_ratio = magnifier_->current().keep_aspect_ratio;
        config_.output_position_px = magnifier_->output_position();
    }
}

void AppHost::save_config() {
    mirror_config_from_controllers();
    store_.save_async(config_);
}

}  // namespace mag
