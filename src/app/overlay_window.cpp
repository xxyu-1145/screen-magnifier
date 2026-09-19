// app/overlay_window.cpp — the topmost magnifier window.
//
// Design note: the client rectangle belongs to the DirectComposition renderer,
// so nothing in this file paints there. Hit testing, the resize band and the
// edge hint are expressed through Win32 styles, WM_NCHITTEST and DWM chrome
// instead of GDI drawing.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// The project builds without -DUNICODE, which would make the IDC_* resource
// macros expand to MAKEINTRESOURCEA and clash with the W entry points used here.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "app/overlay_window.h"

#include <dwmapi.h>

#include <system_error>
#include <utility>

// Both constants are newer than the MinGW-w64 headers zig ships; spell them out
// rather than depend on the SDK version that happens to be installed.
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace mag {
namespace {

constexpr wchar_t kWindowClass[] = L"MagOverlayWindow";

// A session without a desktop reports 0x0 for the virtual screen; bounds this
// wide keep clamp_into() a no-op instead of collapsing every rect to the origin.
constexpr Px kFallbackDesktopExtent = 1000000;

// WS_EX_NOREDIRECTIONBITMAP hands the redirection surface to DirectComposition,
// which is mutually exclusive with WS_EX_LAYERED; adding it would blank the
// magnified content.
constexpr DWORD kOverlayExStyle =
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kOverlayStyle = WS_POPUP;

// DWM attributes added after the SDK these headers describe. The edge hint is
// drawn by DWM's window border because a frameless WS_POPUP has an empty
// non-client area: DefWindowProc's WM_NCPAINT would have nothing to paint into,
// and the client rectangle is owned by DirectComposition.
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmColorDefault = 0xFFFFFFFFu;
// Amber reads as "attention" over any wallpaper; COLORREF is 0x00BBGGRR.
constexpr COLORREF kEdgeHintColor = RGB(0xFF, 0x8C, 0x00);

// Message coordinates arrive as two signed 16-bit halves; the cast through
// short is what sign-extends a negative virtual-desktop coordinate.
Px coord_x(LPARAM lp) noexcept { return static_cast<Px>(static_cast<short>(LOWORD(lp))); }
Px coord_y(LPARAM lp) noexcept { return static_cast<Px>(static_cast<short>(HIWORD(lp))); }

// WM_SIZE/WM_GETMINMAXINFO extents are unsigned halves.
Px extent_x(LPARAM lp) noexcept { return static_cast<Px>(LOWORD(lp)); }
Px extent_y(LPARAM lp) noexcept { return static_cast<Px>(HIWORD(lp)); }

RectPx virtual_desktop_rect() noexcept {
    const Px x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const Px y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const Px w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const Px h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        return RectPx{-kFallbackDesktopExtent, -kFallbackDesktopExtent,
                      kFallbackDesktopExtent, kFallbackDesktopExtent};
    }
    return RectPx{x, y, x + w, y + h};
}

LRESULT hit_test_code(ResizeHandle handle) noexcept {
    switch (handle) {
        case ResizeHandle::Left: return HTLEFT;
        case ResizeHandle::Right: return HTRIGHT;
        case ResizeHandle::Top: return HTTOP;
        case ResizeHandle::Bottom: return HTBOTTOM;
        case ResizeHandle::TopLeft: return HTTOPLEFT;
        case ResizeHandle::TopRight: return HTTOPRIGHT;
        case ResizeHandle::BottomLeft: return HTBOTTOMLEFT;
        case ResizeHandle::BottomRight: return HTBOTTOMRIGHT;
        default: break;
    }
    return HTCLIENT;
}

ResizeHandle handle_from_ht(UINT code) noexcept {
    switch (code) {
        case HTLEFT: return ResizeHandle::Left;
        case HTRIGHT: return ResizeHandle::Right;
        case HTTOP: return ResizeHandle::Top;
        case HTBOTTOM: return ResizeHandle::Bottom;
        case HTTOPLEFT: return ResizeHandle::TopLeft;
        case HTTOPRIGHT: return ResizeHandle::TopRight;
        case HTBOTTOMLEFT: return ResizeHandle::BottomLeft;
        case HTBOTTOMRIGHT: return ResizeHandle::BottomRight;
        case HTCLIENT: return ResizeHandle::Move;
        default: return ResizeHandle::None;
    }
}

LPCWSTR cursor_for_handle(ResizeHandle handle) noexcept {
    switch (handle) {
        case ResizeHandle::Left:
        case ResizeHandle::Right: return IDC_SIZEWE;
        case ResizeHandle::Top:
        case ResizeHandle::Bottom: return IDC_SIZENS;
        case ResizeHandle::TopLeft:
        case ResizeHandle::BottomRight: return IDC_SIZENWSE;
        case ResizeHandle::TopRight:
        case ResizeHandle::BottomLeft: return IDC_SIZENESW;
        case ResizeHandle::Move: return IDC_SIZEALL;
        default: return IDC_ARROW;
    }
}

// value * num / den with round-half-away-from-zero, in int64 so a desktop-sized
// edge at 20x cannot overflow. den is positive by construction.
Px scale_ratio(Px value, Px num, Px den) noexcept {
    if (den == 0) return value;
    const std::int64_t v = static_cast<std::int64_t>(value) * num;
    const std::int64_t d = den;
    const std::int64_t half = d / 2;
    return static_cast<Px>(v >= 0 ? (v + half) / d : (v - half) / d);
}

// Edge-drag geometry: the anchored edges stay put, the grabbed edges follow the
// pointer, the aspect ratio is preserved on Ctrl, the window never falls below
// the contractual minimum and the grabbed edges never leave the desktop.
RectPx resize_rect(const RectPx& origin, ResizeHandle handle, Px dx, Px dy, bool keep_aspect,
                   const RectPx& bounds) noexcept {
    const bool grip_left = handle_has(handle, ResizeHandle::Left);
    const bool grip_top = handle_has(handle, ResizeHandle::Top);
    const bool grip_right = handle_has(handle, ResizeHandle::Right);
    const bool grip_bottom = handle_has(handle, ResizeHandle::Bottom);

    Px left = origin.left;
    Px top = origin.top;
    Px right = origin.right;
    Px bottom = origin.bottom;
    if (grip_left) left += dx;
    if (grip_right) right += dx;
    if (grip_top) top += dy;
    if (grip_bottom) bottom += dy;

    if (keep_aspect) {
        const Px w0 = width_of(origin);
        const Px h0 = height_of(origin);
        if (w0 > 0 && h0 > 0) {
            // Only one axis can be honoured exactly, so follow whichever the
            // pointer moved further relative to the starting size.
            const std::int64_t dw = static_cast<std::int64_t>(right - left - w0) * h0;
            const std::int64_t dh = static_cast<std::int64_t>(bottom - top - h0) * w0;
            if (dw >= dh) {
                const Px nh = scale_ratio(right - left, h0, w0);
                if (grip_top) top = bottom - nh; else bottom = top + nh;
            } else {
                const Px nw = scale_ratio(bottom - top, w0, h0);
                if (grip_left) left = right - nw; else right = left + nw;
            }
        }
    }

    // The dragged edge stops at the minimum instead of the window collapsing.
    if (right - left < kMinOutputEdgePx) {
        if (grip_left) left = right - kMinOutputEdgePx; else right = left + kMinOutputEdgePx;
    }
    if (bottom - top < kMinOutputEdgePx) {
        if (grip_top) top = bottom - kMinOutputEdgePx; else bottom = top + kMinOutputEdgePx;
    }

    // Clamp only the moving edges so the anchored corner never slides.
    if (grip_left && left < bounds.left) left = bounds.left;
    if (grip_top && top < bounds.top) top = bounds.top;
    if (grip_right && right > bounds.right) right = bounds.right;
    if (grip_bottom && bottom > bounds.bottom) bottom = bounds.bottom;

    if (right - left < kMinOutputEdgePx) {
        if (grip_left) left = right - kMinOutputEdgePx; else right = left + kMinOutputEdgePx;
    }
    if (bottom - top < kMinOutputEdgePx) {
        if (grip_top) top = bottom - kMinOutputEdgePx; else bottom = top + kMinOutputEdgePx;
    }

    return RectPx{left, top, right, bottom};
}

void apply_border_color(HWND hwnd, DWORD color) noexcept {
    if (hwnd == nullptr) return;
    DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &color, sizeof(color));
    // Recomputing the frame makes DWM drop its cached border immediately;
    // without it the colour change waits for the next unrelated frame update.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                              SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
}

}  // namespace

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() { destroy(); }

void OverlayWindow::create(HINSTANCE instance, Callbacks callbacks) {
    if (hwnd_ != nullptr) {
        throw std::logic_error("OverlayWindow::create: a window already exists");
    }
    if (instance == nullptr) instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OverlayWindow::wnd_proc_thunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::system_error(static_cast<int>(err), std::system_category(),
                                    "OverlayWindow::create: RegisterClassExW failed");
        }
    }

    instance_ = instance;
    HWND hwnd = CreateWindowExW(kOverlayExStyle, kWindowClass, L"Magnifier", kOverlayStyle, 0, 0,
                                kMinOutputEdgePx, kMinOutputEdgePx, nullptr, nullptr, instance,
                                this);
    if (hwnd == nullptr) {
        const DWORD err = GetLastError();
        hwnd_ = nullptr;
        throw std::system_error(static_cast<int>(err), std::system_category(),
                                "OverlayWindow::create: CreateWindowExW failed");
    }
    // Messages raised during creation (WM_SIZE among them) therefore run with
    // no callbacks installed, so no half-constructed owner is reentered.
    hwnd_ = hwnd;
    callbacks_ = std::move(callbacks);

    // Mirroring ourselves is the failure mode this guards; a caller that must
    // report a refusal can call exclude_from_capture() again and read the bool.
    (void)exclude_from_capture(true);
}

void OverlayWindow::destroy() noexcept {
    if (hwnd_ != nullptr) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        dragging_ = false;
        dragging_resize_ = false;
        active_handle_ = ResizeHandle::None;
        tracking_mouse_ = false;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    instance_ = nullptr;
}

void OverlayWindow::show() noexcept {
    if (hwnd_ == nullptr) return;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // The magnifier must stay above every other window even after another
    // topmost window appeared, and must do so without becoming active.
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OverlayWindow::hide() noexcept {
    if (hwnd_ == nullptr) return;
    // Dropped without callbacks: hide() is noexcept and the state machine
    // already knows the gesture ended.
    if (GetCapture() == hwnd_) ReleaseCapture();
    dragging_ = false;
    dragging_resize_ = false;
    active_handle_ = ResizeHandle::None;
    tracking_mouse_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

void OverlayWindow::apply_geometry(const RectPx& window_rect_px) {
    if (hwnd_ == nullptr || is_empty(window_rect_px)) return;
    const RectPx r = normalize(window_rect_px);
    // WM_SIZE, raised synchronously from here, is what pushes the new client
    // size to the renderer through on_client_size_changed.
    SetWindowPos(hwnd_, nullptr, r.left, r.top, width_of(r), height_of(r),
                 SWP_NOACTIVATE | SWP_NOZORDER);
    std::lock_guard<std::mutex> lock(rect_mutex_);
    last_rect_ = r;
}

void OverlayWindow::apply_interaction_state(InteractionState state) {
    if (hwnd_ == nullptr) return;

    // PassThrough is the only state that hands the mouse to the desktop.
    // EdgeArmed keeps hit testing because the click that leaves it arrives as
    // a mouse-down on this window, and Suspended must not eat the mouse while
    // it shows a stale frame.
    const bool transparent = state == InteractionState::PassThrough ||
                             state == InteractionState::Suspended ||
                             state == InteractionState::Off;
    set_hit_test_transparent(transparent);

    if (state != InteractionState::Off) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void OverlayWindow::set_hit_test_transparent(bool transparent) {
    if (hwnd_ == nullptr) {
        throw std::system_error(static_cast<int>(ERROR_INVALID_WINDOW_HANDLE),
                                std::system_category(),
                                "OverlayWindow::set_hit_test_transparent: no window");
    }
    transparent_.store(transparent, std::memory_order_relaxed);

    // WM_NCHITTEST reads the flag on every mouse move, so all that is left is
    // to repaint the frame. The window is never hidden while transparent.
    if (!RedrawWindow(hwnd_, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW) &&
        IsWindow(hwnd_)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "OverlayWindow::set_hit_test_transparent: invalidate failed");
    }
}

void OverlayWindow::show_edge_hint() noexcept {
    edge_hint_.store(true, std::memory_order_relaxed);
    apply_border_color(hwnd_, kEdgeHintColor);
}

void OverlayWindow::hide_edge_hint() noexcept {
    edge_hint_.store(false, std::memory_order_relaxed);
    apply_border_color(hwnd_, kDwmColorDefault);
}

void OverlayWindow::set_content_stale(bool stale) noexcept {
    content_stale_.store(stale, std::memory_order_relaxed);
    // Only the renderer can blank its own swap chain; this makes the flag
    // visible and asks anyone owning the window surface to repaint.
    if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool OverlayWindow::exclude_from_capture(bool enable) noexcept {
    if (hwnd_ == nullptr) return false;
    // WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004+; an older build returns
    // FALSE with ERROR_INVALID_PARAMETER and the caller has to know.
    return SetWindowDisplayAffinity(hwnd_, enable ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
}

RectPx OverlayWindow::window_rect_px() const noexcept {
    if (hwnd_ != nullptr) {
        RECT rc{};
        // GetWindowRect reports physical pixels for a per-monitor-DPI-v2 process.
        if (GetWindowRect(hwnd_, &rc)) {
            return RectPx{rc.left, rc.top, rc.right, rc.bottom};
        }
    }
    std::lock_guard<std::mutex> lock(rect_mutex_);
    return last_rect_;
}

ResizeHandle OverlayWindow::hit_test_border(PointPx client_px) const {
    RECT rc{};
    if (hwnd_ == nullptr || !GetClientRect(hwnd_, &rc)) return ResizeHandle::None;

    const Px w = rc.right - rc.left;
    const Px h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return ResizeHandle::None;

    // A window near the minimum edge would otherwise be all band and no
    // draggable centre; shrink the band rather than swallow the middle.
    const Px band_x = w >= kResizeBorderPx * 3 ? kResizeBorderPx : w / 3;
    const Px band_y = h >= kResizeBorderPx * 3 ? kResizeBorderPx : h / 3;

    std::uint32_t bits = 0;
    if (client_px.x < rc.left + band_x) {
        bits |= handle_bits(ResizeHandle::Left);
    } else if (client_px.x >= rc.right - band_x) {
        bits |= handle_bits(ResizeHandle::Right);
    }
    if (client_px.y < rc.top + band_y) {
        bits |= handle_bits(ResizeHandle::Top);
    } else if (client_px.y >= rc.bottom - band_y) {
        bits |= handle_bits(ResizeHandle::Bottom);
    }
    return bits == 0 ? ResizeHandle::None : static_cast<ResizeHandle>(bits);
}

void OverlayWindow::begin_drag(PointPx cursor, bool from_caption) {
    if (hwnd_ == nullptr || dragging_) return;

    // The state machine leaves EdgeArmed on any press over the window.
    if (callbacks_.on_interaction) callbacks_.on_interaction();
    if (hwnd_ == nullptr || dragging_) return;

    drag_origin_screen_ = cursor;
    drag_origin_rect_ = window_rect_px();
    dragging_ = true;
    dragging_resize_ = !from_caption;
    SetCapture(hwnd_);

    if (from_caption) {
        active_handle_ = ResizeHandle::None;
        if (callbacks_.on_drag_begin) callbacks_.on_drag_begin(cursor);
    } else {
        if (callbacks_.on_resize_begin) callbacks_.on_resize_begin(active_handle_, cursor);
    }
}

void OverlayWindow::update_drag(PointPx cursor, bool ctrl_down) {
    if (!dragging_ || hwnd_ == nullptr) return;

    // Every update is resolved from the rect at drag start, so the result never
    // accumulates an error and a clamp cannot be walked back.
    const Px dx = cursor.x - drag_origin_screen_.x;
    const Px dy = cursor.y - drag_origin_screen_.y;
    const RectPx bounds = virtual_desktop_rect();
    const RectPx before = window_rect_px();

    RectPx next;
    if (dragging_resize_) {
        next = resize_rect(drag_origin_rect_, active_handle_, dx, dy, ctrl_down, bounds);
    } else {
        const RectPx moved{drag_origin_rect_.left + dx, drag_origin_rect_.top + dy,
                           drag_origin_rect_.right + dx, drag_origin_rect_.bottom + dy};
        next = clamp_into(moved, bounds);
    }

    if (next != before) apply_geometry(next);

    if (dragging_resize_) {
        if (callbacks_.on_resize_update) {
            callbacks_.on_resize_update(active_handle_, PointPx{dx, dy});
        }
    } else {
        if (callbacks_.on_drag_update) callbacks_.on_drag_update(cursor);
        if ((next.left != before.left || next.top != before.top) &&
            callbacks_.on_position_changed) {
            callbacks_.on_position_changed(PointPx{next.left, next.top});
        }
    }
}

void OverlayWindow::end_drag() {
    if (!dragging_) return;

    // Cleared before releasing capture: ReleaseCapture re-enters through
    // WM_CAPTURECHANGED, which must not find an armed drag.
    const bool was_resize = dragging_resize_;
    const bool was_dragging = dragging_;
    dragging_ = false;
    dragging_resize_ = false;
    active_handle_ = ResizeHandle::None;
    if (hwnd_ != nullptr && GetCapture() == hwnd_) ReleaseCapture();

    if (!was_dragging) return;
    if (was_resize) {
        if (callbacks_.on_resize_end) callbacks_.on_resize_end();
    } else {
        if (callbacks_.on_drag_end) callbacks_.on_drag_end();
    }
}

LRESULT CALLBACK OverlayWindow::wnd_proc_thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OverlayWindow* self =
        reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr && msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->wnd_proc(hwnd, msg, wp, lp);
}

LRESULT OverlayWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp;
    switch (msg) {
        case WM_NCCREATE:
            hwnd_ = hwnd;
            break;

        case WM_MOUSEACTIVATE:
            // Clicking must never pull focus away from the magnified game.
            return MA_NOACTIVATE;

        case WM_NCHITTEST: {
            if (transparent_.load(std::memory_order_relaxed)) {
                // The desktop below keeps the mouse; the window stays visible.
                return HTTRANSPARENT;
            }
            POINT pt{coord_x(lp), coord_y(lp)};
            if (!ScreenToClient(hwnd, &pt)) return HTCLIENT;
            return hit_test_code(hit_test_border(PointPx{pt.x, pt.y}));
        }

        case WM_SETCURSOR: {
            const ResizeHandle handle =
                dragging_ ? (dragging_resize_ ? active_handle_ : ResizeHandle::Move)
                          : handle_from_ht(LOWORD(lp));
            if (handle == ResizeHandle::None) break;
            SetCursor(LoadCursorW(nullptr, cursor_for_handle(handle)));
            return TRUE;
        }

        case WM_LBUTTONDOWN:
        case WM_NCLBUTTONDOWN: {
            // A hit-test code other than HTCLIENT makes Windows deliver the
            // press as a non-client message, which carries screen coordinates.
            POINT screen{coord_x(lp), coord_y(lp)};
            POINT client = screen;
            if (msg == WM_LBUTTONDOWN) {
                if (!ClientToScreen(hwnd, &screen)) break;
            } else {
                if (!ScreenToClient(hwnd, &client)) break;
            }
            const ResizeHandle handle = hit_test_border(PointPx{client.x, client.y});
            active_handle_ = handle;
            // With no caption the whole client area acts as one, so a press
            // outside the resize band moves the window.
            begin_drag(PointPx{screen.x, screen.y}, handle == ResizeHandle::None);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                tracking_mouse_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (dragging_) {
                POINT screen{coord_x(lp), coord_y(lp)};
                if (ClientToScreen(hwnd, &screen)) {
                    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    update_drag(PointPx{screen.x, screen.y}, ctrl);
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            return 0;

        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            if (dragging_) {
                end_drag();
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            if (dragging_ && reinterpret_cast<HWND>(lp) != hwnd) end_drag();
            break;

        case WM_SIZE: {
            RECT rc{};
            if (GetWindowRect(hwnd, &rc)) {
                std::lock_guard<std::mutex> lock(rect_mutex_);
                last_rect_ = RectPx{rc.left, rc.top, rc.right, rc.bottom};
            }
            if (callbacks_.on_client_size_changed) {
                callbacks_.on_client_size_changed(SizePx{extent_x(lp), extent_y(lp)});
            }
            return 0;
        }

        case WM_MOVE: {
            RECT rc{};
            if (GetWindowRect(hwnd, &rc)) {
                std::lock_guard<std::mutex> lock(rect_mutex_);
                last_rect_ = RectPx{rc.left, rc.top, rc.right, rc.bottom};
            }
            return 0;
        }

        case WM_ERASEBKGND:
            // Every pixel is owned by the DirectComposition renderer.
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_CLOSE:
            // Closing is a request: capture has to stop before the window goes.
            if (callbacks_.on_close_requested) callbacks_.on_close_requested();
            return 0;

        case WM_NCDESTROY:
            if (hwnd_ == hwnd) hwnd_ = nullptr;
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace mag
