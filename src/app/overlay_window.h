// app/overlay_window.h — the topmost magnifier window.
//
// Owns hit testing, click-through, edge-dwell hinting and the three window
// resize paths (edge drag, keyboard steps, numeric entry).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "core/event_bus.h"
#include "core/interaction_state_machine.h"
#include "core/types.h"

namespace mag {

class OverlayWindow final : public IOverlayWindow {
public:
    // Callbacks are invoked on the UI thread from inside the window procedure.
    struct Callbacks {
        std::function<void(PointPx)> on_drag_begin;      // move drag started
        std::function<void(PointPx)> on_drag_update;     // move drag in progress
        std::function<void()> on_drag_end;
        std::function<void(ResizeHandle, PointPx)> on_resize_begin;
        std::function<void(ResizeHandle, PointPx)> on_resize_update;
        std::function<void()> on_resize_end;
        std::function<void(SizePx)> on_client_size_changed;
        std::function<void()> on_close_requested;
        std::function<void()> on_interaction;            // any mouse-down
        std::function<void(PointPx)> on_position_changed;
    };

    OverlayWindow();
    ~OverlayWindow() override;

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Registers the window class and creates the window hidden.
    // Throws std::system_error when creation fails.
    void create(HINSTANCE instance, Callbacks callbacks);

    void destroy() noexcept;

    HWND hwnd() const noexcept { return hwnd_; }

    // Applies the window rect (physical pixels, virtual-desktop coordinates)
    // and pushes the new client size to the renderer.
    void apply_geometry(const RectPx& window_rect_px);

    // Applies pass-through / interactive hit testing and the topmost state.
    void apply_interaction_state(InteractionState state);

    // --- IOverlayWindow ---
    void show() noexcept override;
    void hide() noexcept override;
    void set_hit_test_transparent(bool transparent) override;
    void show_edge_hint() noexcept override;
    void hide_edge_hint() noexcept override;

    // Tells the renderer to blank the window (capture suspended).
    void set_content_stale(bool stale) noexcept;

    // Excludes this window from desktop capture so the magnifier cannot mirror
    // itself. Returns false when the OS refuses.
    bool exclude_from_capture(bool enable) noexcept;

    // The whole window rectangle in virtual-desktop physical pixels.
    RectPx window_rect_px() const noexcept;

    // The border band used for resize grabs, in physical pixels.
    static constexpr Px kResizeBorderPx = 8;

private:
    static LRESULT CALLBACK wnd_proc_thunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT wnd_proc(HWND, UINT, WPARAM, LPARAM);

    void begin_drag(PointPx cursor, bool from_caption);
    void update_drag(PointPx cursor, bool ctrl_down);
    void end_drag();
    ResizeHandle hit_test_border(PointPx client_px) const;

    HWND hwnd_{nullptr};
    HINSTANCE instance_{nullptr};
    Callbacks callbacks_{};
    std::atomic<bool> transparent_{false};
    std::atomic<bool> edge_hint_{false};
    std::atomic<bool> content_stale_{false};
    bool tracking_mouse_{false};
    bool dragging_{false};
    bool dragging_resize_{false};
    ResizeHandle active_handle_{ResizeHandle::None};
    PointPx drag_origin_screen_{};
    RectPx drag_origin_rect_{};
    mutable std::mutex rect_mutex_;
    RectPx last_rect_{};
};

}  // namespace mag
