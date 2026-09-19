// app/selection_overlay.h — the full-desktop region picker.
//
// A dimmed, click-through-free overlay spanning the whole virtual desktop that
// lets the user drag out a source region, switch shape, nudge by single pixels
// and preview the result through a loupe before committing.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>

#include "core/types.h"

namespace mag {

class SelectionOverlay {
public:
    struct Callbacks {
        std::function<void(const SelectionConfig&)> on_committed;
        std::function<void(const SelectionConfig&)> on_preview;
        std::function<void(SelectionShape)> on_shape_changed;
        std::function<void()> on_cancelled;
    };

    SelectionOverlay();
    ~SelectionOverlay();

    SelectionOverlay(const SelectionOverlay&) = delete;
    SelectionOverlay& operator=(const SelectionOverlay&) = delete;

    // Creates the overlay across `virtual_desktop_px`. It starts hidden.
    void create(HINSTANCE instance, RectPx virtual_desktop_px, Callbacks callbacks);

    void destroy() noexcept;

    // Shows the overlay for picking, seeded with the current selection.
    void open(const SelectionConfig& initial);

    // Hides without committing.
    void cancel();

    bool active() const noexcept { return active_; }

    // Re-applies the virtual desktop extent after a topology change.
    void set_virtual_desktop(RectPx rect_px);

    // The live geometry of the in-progress selection.
    const SelectionConfig& draft() const noexcept { return draft_; }

private:
    static LRESULT CALLBACK wnd_proc_thunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT wnd_proc(HWND, UINT, WPARAM, LPARAM);

    void paint();
    // Where the confirm and cancel buttons landed on the last paint, in client
    // coordinates. Painting computes them and hit testing reads them, so there
    // is one description of where they are rather than two that can disagree.
    bool over_hint_button(POINT client_pt, bool& is_ok) const noexcept;
    void rebuild_backbuffer();
    // Frees the full-desktop DIB. Called when the picker closes, not only when
    // it is destroyed: a 32-bit buffer the size of the virtual desktop is tens
    // of megabytes of idle cost.
    void release_backbuffer() noexcept;
    void commit();
    void cycle_shape(int direction);
    void nudge(Px dx, Px dy);
    void update_loupe();

    HWND hwnd_{nullptr};
    HINSTANCE instance_{nullptr};
    Callbacks callbacks_{};
    RectPx virtual_desktop_px_{};
    SelectionConfig draft_{};
    SelectionConfig start_{};
    PointPx anchor_px_{};
    bool dragging_{false};
    bool active_{false};
    bool has_selection_{false};

    // GDI double buffer sized to the overlay's client area.
    HDC mem_dc_{nullptr};
    HBITMAP mem_bitmap_{nullptr};
    HBITMAP mem_bitmap_old_{nullptr};
    void* mem_bits_{nullptr};
    RECT hint_bar_{};
    RECT ok_button_{};
    RECT cancel_button_{};
    SizePx buffer_size_{};
};

}  // namespace mag
