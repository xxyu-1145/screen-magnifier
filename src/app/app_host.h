// app/app_host.h — composition root.
//
// Owns every subsystem, wires the event flow described in design doc §2.1 and
// runs the UI message loop that doubles as the control thread.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/control_window.h"
#include "app/overlay_window.h"
#include "app/selection_overlay.h"
#include "app/ui_draw.h"
#include "capture/capture.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/interaction_state_machine.h"
#include "core/magnification_controller.h"
#include "core/selection_controller.h"
#include "platform/input.h"
#include "platform/topology.h"
#include "platform/tray.h"
#include "render/render_service.h"

namespace mag {

// Owns one capture backend per monitor and gives the state machine a single
// start/stop/recover handle.
//
// DXGI Desktop Duplication is per-output, so a magnifier that spans screens
// needs a session per screen. Starting them all is what makes a cross-monitor
// selection show live content on both halves.
class CaptureGroup final : public ICaptureControl {
public:
    CaptureGroup(BoundedEventBus& bus, Win32TopologyService& topology);
    ~CaptureGroup() override;

    CaptureGroup(const CaptureGroup&) = delete;
    CaptureGroup& operator=(const CaptureGroup&) = delete;

    // Creates the backends for the given monitors. Falls back to GDI capture
    // per monitor when duplication is refused, recording why.
    void build(const std::vector<MonitorInfoPx>& monitors);

    // Drops every session; used when the monitor set changes.
    void rebuild(const std::vector<MonitorInfoPx>& monitors);

    void start() noexcept override;
    void stop() noexcept override;
    void recover_async() noexcept override;

    std::vector<ICaptureBackend*> raw() const;
    bool running() const noexcept { return started_; }
    bool using_fallback() const noexcept { return fallback_; }
    const std::string& note() const noexcept { return note_; }
    const std::string& last_error() const noexcept { return last_error_; }
    std::string backend_summary() const;

private:
    BoundedEventBus& bus_;
    Win32TopologyService& topology_;
    std::vector<std::unique_ptr<ICaptureBackend>> backends_;
    // Parallel to backends_: the monitor each backend was opened for, so a
    // stopped session can be restarted without re-querying the topology.
    std::vector<MonitorInfoPx> backend_monitors_;
    bool started_{false};
    bool fallback_{false};
    std::string note_;
    std::string last_error_;
    std::string summary_;
};

class AppHost {
public:
    explicit AppHost(HINSTANCE instance);
    ~AppHost();

    AppHost(const AppHost&) = delete;
    AppHost& operator=(const AppHost&) = delete;

    // Loads config, brings up DPI awareness, creates windows and services.
    // Returns false (with a message in last_error()) when startup must abort.
    bool initialize();

    // Runs the message loop until the user quits.
    int run();

    void shutdown() noexcept;

    const std::string& last_error() const noexcept { return last_error_; }

private:
    // --- event handling, all on the UI thread ---
    void pump_events();
    void handle_event(const AppEvent& event);
    void handle_hotkey(HotkeyAction action);
    void handle_command(unsigned command);

    // --- state publishing ---
    void publish_snapshot();
    void apply_geometry_to_window();
    void refresh_status();
    void push_ui_state();

    // --- operations ---
    void toggle_magnifier();
    void set_pass_through(bool enable);
    void set_shape(SelectionShape shape);
    void cycle_shape();
    void set_factor(Q16 factor);
    void step_factor(int steps);
    void set_output_size(SizePx size);
    void commit_selection(const SelectionConfig& sel);
    void open_picker();
    void on_topology_changed();
    void handle_capture_lost(std::uint32_t output_id);
    void handle_capture_recovered(std::uint32_t output_id);
    void save_config();
    void quit();
    void reset_selection();
    // Puts the magnifier window back in the middle of the primary screen.
    void centre_output_window();
    // Puts a kept region back in force, and remembers it as the save target.
    void recall_selection_slot(int index);
    // Writes the region in force into the slot being recalled, or the first one
    // when none is.
    void save_selection_slot();
    // Re-registers the chords and settles the low-level fallback and the notice
    // that goes with it. The one place either happens.
    void reregister_hotkeys();
    // Puts every setting back to the shipped default and re-applies it.
    void restore_defaults();
    // Centres a region of the given size on the primary monitor.
    RectPx centre_on_primary(Px width, Px height) const;
    void load_config_or_defaults();
    // Copies the live controller state into the config mirror. Called whenever
    // a controller reports a change, so the settings window and the saved file
    // never lag behind the domain state.
    void mirror_config_from_controllers();

    // A message-only window exists purely to receive WM_TIMER reliably.
    // A thread timer created with SetTimer(nullptr, ...) is not guaranteed to
    // have its callback invoked from a DispatchMessage loop, which silently
    // stops the whole control pump; a real window removes that doubt.
    static LRESULT CALLBACK pump_wnd_proc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE instance_{nullptr};
    std::string last_error_;
    // GDI+ must be running before any window paints; it is a member so its
    // lifetime covers every window the host creates and destroys.
    ui::GdiPlusScope gdiplus_;

    AppConfig config_{};
    ConfigStore store_;
    BoundedEventBus bus_{2048};
    SnapshotMailbox mailbox_;
    Win32TopologyService topology_;
    TopologySnapshot topo_{};

    std::unique_ptr<SelectionController> selection_;
    std::unique_ptr<MagnificationController> magnifier_;
    std::unique_ptr<CaptureGroup> captures_;
    std::unique_ptr<InteractionStateMachine> fsm_;

    OverlayWindow overlay_;
    SelectionOverlay picker_;
    ControlWindow control_;
    TrayIcon tray_;

    std::unique_ptr<InputThread> input_;
    std::unique_ptr<RenderService> render_;
    std::jthread render_thread_;

    UINT_PTR pump_timer_{0};
    HWND pump_hwnd_{nullptr};
    std::atomic<bool> shutting_down_{false};
    std::string status_text_;
    // A transient message (capture lost, hotkey conflicts) that survives
    // refresh_status(), which otherwise rebuilds the status line from scratch.
    std::string notice_;
    // Kept apart from notice_ so a capture message cannot overwrite a renderer
    // failure: losing that message would hide the reason the window is blank.
    std::string render_error_;
    bool suspended_{false};
    // True when there was no configuration file, so the shipped defaults are in
    // force and the first-run placement rules apply.
    bool fresh_config_{false};
    bool exclude_from_capture_active_{false};
    // Written by the render thread when it fails, read by the UI thread; the
    // bus carries the message and this flag only says "redraw".
    std::atomic<bool> ui_dirty_{false};
    // The settings window shows live counters, so it is refreshed on a timer as
    // well as on events; otherwise the readout freezes the moment the user
    // stops clicking while the numbers keep moving underneath it.
    std::uint64_t last_ui_refresh_qpc_{0};
    // Windows keeps freed pages resident until something asks for them back, so
    // the idle-memory budget only holds if the process explicitly trims once it
    // has finished releasing the GPU session.
    bool working_set_trimmed_{false};
    // Trimming before anything has been allocated only evicts hot code pages,
    // so it is deferred until the magnifier has actually been used once.
    bool ever_started_{false};
    bool diagnostics_{false};
};

}  // namespace mag
