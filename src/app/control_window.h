// app/control_window.h — the settings and status window.
//
// Every control is created programmatically: the zig toolchain has no resource
// compiler, and keeping the layout in code makes the UI diffable.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <functional>
#include <string>

#include "core/config.h"
#include "core/types.h"
#include "render/render_service.h"

namespace mag {

class ControlWindow {
public:
    struct Callbacks {
        std::function<void()> on_pick_region;
        std::function<void()> on_toggle_magnifier;
        std::function<void()> on_toggle_pass_through;
        std::function<void(SelectionShape)> on_shape_changed;
        std::function<void(int)> on_selection_slot;
        std::function<void()> on_save_selection;
        std::function<void(Q16)> on_factor_changed;
        // The four preset factors, after one of their fields was edited.
        std::function<void(const std::array<Q16, kPresetCount>&)> on_presets_changed;
        std::function<void(SizePx)> on_output_size_changed;
        std::function<void(bool)> on_keep_aspect_changed;
        std::function<void(PointPx)> on_output_position_changed;
        std::function<void(const std::array<HotkeyChord, kHotkeyCount>&)> on_hotkeys_changed;
        // True while the settings window is waiting for a chord to be typed.
        // RegisterHotKey consumes the chords this program owns, so they have to
        // be released for the duration or the one the user is most likely to
        // try -- one it already holds -- would fire its action instead.
        std::function<void(bool)> on_chord_capture;
        std::function<void(bool)> on_strict_compat_changed;
        std::function<void(Px, int, bool)> on_edge_dwell_changed;
        std::function<void(ScaleFilter)> on_filter_changed;
        std::function<void()> on_save_config;
        std::function<void()> on_restore_defaults;
        std::function<void()> on_quit;
        std::function<void()> on_recover_capture;
        std::function<void(bool)> on_exclude_from_capture_changed;
        std::function<void(bool)> on_show_border_changed;
        // Raised when the user switches language from the header toggle.
        std::function<void(Language)> on_language_changed;
    };

    ControlWindow();
    ~ControlWindow();

    ControlWindow(const ControlWindow&) = delete;
    ControlWindow& operator=(const ControlWindow&) = delete;

    // Throws std::system_error when the window cannot be created.
    void create(HINSTANCE instance, Callbacks callbacks, const std::function<void()>& on_closed);

    void destroy() noexcept;

    HWND hwnd() const noexcept { return hwnd_; }

    void show();
    void hide();
    void bring_to_front();

    // Pushes current state into the widgets without firing callbacks.
    void sync(const AppConfig& cfg, InteractionState state, const RenderStats& stats,
              const RectPx& selection_px, const RectPx& output_px, const std::string& status,
              const std::string& capture_backend, bool capture_fallback);

    // Replaces the status line only; cheaper than a full sync.
    void set_status(const std::string& status);
    void set_capture_state(const std::string& backend, bool fallback, bool suspended);

    // Re-applies every localised string after the language changes.
    void retranslate();

    // Chord capture. The hotkey edit boxes own the keyboard while a capture is
    // pending, so these are driven from their window subclass rather than from
    // the parent's message loop.
    void begin_chord_capture(HWND edit);
    void cancel_chord_capture();
    void handle_chord_key(UINT virtual_key);
    // A mouse button pressed while a field is waiting. The buttons the system
    // charges nothing for -- right, middle and the two side buttons -- are
    // bindable; the left button is how the field is clicked into, so binding it
    // would make the arming gesture start the capture instead of ending it.
    void handle_chord_mouse_button(UINT message, WPARAM wparam);
    bool capturing_chord() const noexcept { return chord_capture_index_ >= 0; }
    // True when `edit` is the field a capture is currently armed on. Used by the
    // field's own subclass so losing focus to a *different* field cannot disarm
    // the one being entered.
    bool is_armed_field(HWND edit) const noexcept;

    // Reads the factor box, clamps it into the legal range and applies it.
    // `rewrite` puts the canonical text back; it is left alone while the user is
    // still typing, so the caret does not jump mid-number.
    void commit_factor_edit(bool rewrite);
    // The same for one of the preset fields: `index` is the slot, `rewrite` puts
    // the canonical text back once the user is done with it.
    void commit_preset_edit(int index, bool rewrite);

    // Hover tracking for the owner-drawn buttons.
    HWND hovered() const noexcept;
    void set_hovered(HWND control) noexcept;

private:
    static LRESULT CALLBACK wnd_proc_thunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT wnd_proc(HWND, UINT, WPARAM, LPARAM);

    void build_controls();
    void layout_controls(int client_w, int client_h, bool resize_window);
    void apply_font();
    void refresh_hotkey_labels();
    void refresh_factor_label();
    // The only place the factor box is written, so the "we wrote this" flag
    // that keeps its EN_CHANGE from being read as typing cannot be forgotten.
    void write_factor_text();
    void write_preset_text(int index);
    void refresh_preset_labels();
    void refresh_filter_labels();
    void on_command(int control_id, int notify_code);
    // Stores the chord a capture ended on, with whatever modifiers are held,
    // and hands it to the rebind callback. Shared by the keyboard path and the
    // mouse one so the two cannot drift apart.
    void commit_captured_chord(UINT virtual_key);
    // Reads the width/height boxes, clamps them and reports the result.
    void commit_size_edits();
    // Turns a size-slider position into an output size and reports it.
    void apply_size_slider(Px width);
    // Clamped edge-dwell time in milliseconds, read from its field.
    int read_dwell_ms() const;

    HWND hwnd_{nullptr};
    HINSTANCE instance_{nullptr};
    Callbacks callbacks_{};
    std::function<void()> on_closed_{};

    struct Impl;
    Impl* impl_{nullptr};

    AppConfig cfg_{};
    int chord_capture_index_{-1};
    bool sized_once_{false};
};

}  // namespace mag
