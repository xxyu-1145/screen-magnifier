// app/selection_overlay.cpp — the full-desktop region picker (design doc §3.1).
//
// The overlay is a WS_EX_LAYERED popup spanning the whole virtual desktop. Its
// pixels are authored by hand into a 32bpp premultiplied-ARGB DIB section and
// presented with UpdateLayeredWindow; that is the only way to get a genuinely
// translucent dim over the live desktop instead of a colour-keyed fake.
//
// GDI never writes an alpha channel, so every drawing pass leaves a zero alpha
// byte and one fix-up sweep at the end of paint() promotes the decoration
// pixels to opaque while leaving the dim (alpha 0x80) and the cleared selection
// hole (0x00000000) alone. The consequence is that no decoration may use pure
// black: (0,0,0,0) is exactly what "fully transparent" looks like.

#include "app/selection_overlay.h"

#include "core/i18n.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>

#include "core/hit_test.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace mag {
namespace {

constexpr wchar_t kOverlayClassName[] = L"MagSelectionOverlayWindow";
constexpr wchar_t kPaintTickProp[] = L"MagSelectionOverlayPaintTick";
constexpr UINT_PTR kPaintTimerId = 1u;
constexpr ULONGLONG kRepaintIntervalMs = 33u;  // ~30 Hz, per the picker spec

// 50% black, already premultiplied: BGRA bytes 0,0,0,128.
constexpr std::uint32_t kDimArgb = 0x80000000u;

constexpr Px kOutlineWidthPx = 2;
constexpr Px kHandleHalfPx = 3;  // drawn grab handle is 7x7 physical pixels
constexpr Px kLoupeRadiusPx = 64;
constexpr Px kLoupeZoom = 8;  // integer zoom keeps a source pixel exactly 8x8
constexpr Px kLoupeGapPx = 16;
constexpr Px kLabelGapPx = 12;
constexpr Px kLabelPadXPx = 9;
constexpr Px kLabelPadYPx = 5;

constexpr COLORREF kAccent = RGB(0x4C, 0xA8, 0xFF);
constexpr COLORREF kOutlineShadow = RGB(0x08, 0x0C, 0x12);
constexpr COLORREF kHandleFill = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kHandleEdge = RGB(0x12, 0x16, 0x1C);
constexpr COLORREF kLabelBg = RGB(0x16, 0x1A, 0x22);
constexpr COLORREF kLabelEdge = RGB(0x38, 0x42, 0x50);
constexpr COLORREF kLabelFg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kCrosshair = RGB(0xFF, 0x3B, 0x30);
constexpr COLORREF kLoupeRim = RGB(0xE6, 0xEE, 0xFF);

inline Px abs_px(Px v) noexcept { return v < 0 ? -v : v; }

// Virtual-desktop coordinates -> client coordinates.
inline RECT rect_to_client(const RectPx& r, const RectPx& origin) noexcept {
    RECT out{};
    out.left = r.left - origin.left;
    out.top = r.top - origin.top;
    out.right = r.right - origin.left;
    out.bottom = r.bottom - origin.top;
    return out;
}

// Mouse lpARAM -> virtual-desktop physical pixels. The client origin of the
// overlay is the virtual-desktop origin, so the two differ by a pure offset.
inline PointPx point_from_lparam(LPARAM lp, const RectPx& origin) noexcept {
    const Px x = static_cast<Px>(static_cast<short>(LOWORD(lp))) + origin.left;
    const Px y = static_cast<Px>(static_cast<short>(HIWORD(lp))) + origin.top;
    return PointPx{x, y};
}

// The shape actually drawn and hit-tested. A circle is centred in the
// selection rect and uses min(w,h) as its diameter, so the preview outline and
// the magnified mask can never disagree.
RectPx visual_bounds(const SelectionConfig& sel) noexcept {
    if (sel.shape != SelectionShape::Circle) return sel.bounds_px;
    const Px w = width_of(sel.bounds_px);
    const Px h = height_of(sel.bounds_px);
    const Px side = w < h ? w : h;
    const Px left = sel.bounds_px.left + (w - side) / 2;
    const Px top = sel.bounds_px.top + (h - side) / 2;
    return RectPx{left, top, left + side, top + side};
}

// Circle drags keep the corner the user grabbed pinned and grow a square, so
// the gesture reads as "circle grows from where I pressed".
RectPx square_about_anchor(PointPx anchor, PointPx cursor) noexcept {
    const Px dx = cursor.x - anchor.x;
    const Px dy = cursor.y - anchor.y;
    const Px side = std::min(abs_px(dx), abs_px(dy));
    return normalize(RectPx{anchor.x, anchor.y, anchor.x + (dx < 0 ? -side : side),
                            anchor.y + (dy < 0 ? -side : side)});
}

// Switching to a circle preserves the centre of interest rather than a corner.
RectPx square_about_center(const RectPx& r) noexcept {
    const Px w = width_of(r);
    const Px h = height_of(r);
    const Px side = w < h ? w : h;
    const Px left = r.left + (w - side) / 2;
    const Px top = r.top + (h - side) / 2;
    return RectPx{left, top, left + side, top + side};
}

// Clamps every edge independently (a resize must shrink at the desktop border,
// never slide the whole rect the way clamp_into() does for a move).
RectPx clamp_edges(RectPx r, const RectPx& bounds) noexcept {
    r = normalize(r);
    if (r.left < bounds.left) r.left = bounds.left;
    if (r.top < bounds.top) r.top = bounds.top;
    if (r.right > bounds.right) r.right = bounds.right;
    if (r.bottom > bounds.bottom) r.bottom = bounds.bottom;
    return normalize(r);
}

// Geometry rules that follow from a shape change: a circle equalises its axes
// and a rounded rectangle needs a radius that is actually visible.
void conform_after_shape_change(SelectionConfig& sel) noexcept {
    if (sel.shape == SelectionShape::Circle) {
        sel.bounds_px = square_about_center(sel.bounds_px);
    }
    if (sel.shape == SelectionShape::RoundedRectangle && sel.corner_radius_px <= 0) {
        const Px limit = std::min(width_of(sel.bounds_px), height_of(sel.bounds_px)) / 2;
        sel.corner_radius_px = limit > 0 ? std::min<Px>(24, limit) : 0;
    }
    sel.corner_radius_px = clamp_corner_radius(sel.corner_radius_px, sel.bounds_px);
}

// The localised name comes from the shared table so the picker and the
// settings window can never disagree about what a shape is called.
const wchar_t* localized_shape_label(SelectionShape s) noexcept {
    return tr(shape_label(s));
}

void fill_dim(std::uint32_t* bits, SizePx size) noexcept {
    const std::size_t row = static_cast<std::size_t>(size.width);
    const std::size_t count = row * static_cast<std::size_t>(size.height);
    if (row == 0 || count == 0) return;
    for (std::size_t i = 0; i < row; ++i) bits[i] = kDimArgb;
    for (std::size_t done = row; done < count; done += row) {
        std::memcpy(bits + done, bits, row * sizeof(std::uint32_t));
    }
}

// Promotes every GDI-drawn pixel to opaque. Pixels that are still (0,0,0,0)
// are the cleared selection hole; pixels at alpha 0x80 with a zero RGB triplet
// are untouched dim.
void fix_alpha(std::uint32_t* bits, SizePx size) noexcept {
    const std::size_t count = static_cast<std::size_t>(size.width) *
                              static_cast<std::size_t>(size.height);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t v = bits[i];
        const std::uint32_t a = v >> 24;
        if ((a == 0u || a == 0x80u) && (v & 0x00FFFFFFu) != 0u) {
            bits[i] = v | 0xFF000000u;
        }
    }
}

// The loupe interior is a straight screen read-back: force it fully opaque so
// an all-black source pixel cannot punch a hole through the zoom.
void force_opaque_in_ellipse(std::uint32_t* bits, SizePx size, Px cx, Px cy, Px rx,
                             Px ry) noexcept {
    if (rx <= 0 || ry <= 0) return;
    const Px x0 = std::max<Px>(0, cx - rx);
    const Px x1 = std::min<Px>(size.width, cx + rx);
    const Px y0 = std::max<Px>(0, cy - ry);
    const Px y1 = std::min<Px>(size.height, cy + ry);
    const std::int64_t rx2 = static_cast<std::int64_t>(rx) * rx;
    const std::int64_t ry2 = static_cast<std::int64_t>(ry) * ry;
    const std::int64_t limit = rx2 * ry2;
    for (Px y = y0; y < y1; ++y) {
        const std::int64_t dy = y - cy;
        const std::int64_t dy2 = dy * dy;
        std::uint32_t* row = bits + static_cast<std::size_t>(y) * size.width;
        for (Px x = x0; x < x1; ++x) {
            const std::int64_t dx = x - cx;
            if (dx * dx * ry2 + dy2 * rx2 <= limit) {
                row[x] |= 0xFF000000u;
            }
        }
    }
}

HRGN make_shape_region(const SelectionConfig& sel, const RectPx& origin) noexcept {
    const RectPx v = visual_bounds(sel);
    const RECT wr = rect_to_client(v, origin);
    if (wr.right <= wr.left || wr.bottom <= wr.top) return nullptr;
    switch (sel.shape) {
        case SelectionShape::Circle:
        case SelectionShape::Ellipse:
            return CreateEllipticRgn(wr.left, wr.top, wr.right, wr.bottom);
        case SelectionShape::RoundedRectangle: {
            const Px radius = clamp_corner_radius(sel.corner_radius_px, v);
            if (radius > 0) {
                return CreateRoundRectRgn(wr.left, wr.top, wr.right, wr.bottom, radius * 2,
                                          radius * 2);
            }
            break;
        }
        case SelectionShape::Rectangle:
            break;
    }
    return CreateRectRgn(wr.left, wr.top, wr.right, wr.bottom);
}

// Draws the live outline of the actual shape, so circle / ellipse / rounded
// rect users see the exact mask they are about to magnify.
void stroke_shape(HDC dc, const SelectionConfig& sel, const RectPx& origin, HPEN pen) noexcept {
    const RectPx v = visual_bounds(sel);
    const RECT wr = rect_to_client(v, origin);
    if (wr.right <= wr.left || wr.bottom <= wr.top) return;
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (sel.shape) {
        case SelectionShape::Circle:
        case SelectionShape::Ellipse:
            Ellipse(dc, wr.left, wr.top, wr.right, wr.bottom);
            break;
        case SelectionShape::RoundedRectangle: {
            const Px radius = clamp_corner_radius(sel.corner_radius_px, v);
            if (radius > 0) {
                RoundRect(dc, wr.left, wr.top, wr.right, wr.bottom, radius * 2, radius * 2);
                break;
            }
            [[fallthrough]];
        }
        case SelectionShape::Rectangle:
            Rectangle(dc, wr.left, wr.top, wr.right, wr.bottom);
            break;
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

void draw_handles(HDC dc, const RectPx& bounds, const RectPx& origin, HBRUSH face,
                  HBRUSH edge) noexcept {
    const RECT wr = rect_to_client(bounds, origin);
    if (wr.right < wr.left || wr.bottom < wr.top) return;
    const LONG l = wr.left;
    const LONG t = wr.top;
    const LONG r = wr.right - 1;
    const LONG b = wr.bottom - 1;
    const LONG mx = (l + r) / 2;
    const LONG my = (t + b) / 2;
    const POINT pts[8] = {{l, t}, {mx, t}, {r, t}, {l, my},
                          {r, my}, {l, b}, {mx, b}, {r, b}};
    for (const POINT& p : pts) {
        RECT h{p.x - kHandleHalfPx, p.y - kHandleHalfPx, p.x + kHandleHalfPx + 1,
               p.y + kHandleHalfPx + 1};
        FillRect(dc, &h, face);
        FrameRect(dc, &h, edge);
    }
}

// A bar across the bottom of the desktop carrying the instructions and two
// buttons. It exists because relying on the Enter key alone made confirming a
// region depend on this window actually holding focus, which Windows is free to
// refuse; a mouse target always works.
void draw_hint_bar(HDC dc, SizePx client, RECT& bar_out, RECT& ok_out, RECT& cancel_out) noexcept {
    bar_out = RECT{};
    ok_out = RECT{};
    cancel_out = RECT{};

    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    const wchar_t* hint = tr(Str::PickerHint);
    const wchar_t* ok_text = tr(Str::Ok);
    const wchar_t* cancel_text = tr(Str::Cancel);

    SIZE hint_size{};
    SIZE ok_size{};
    SIZE cancel_size{};
    GetTextExtentPoint32W(dc, hint, static_cast<int>(std::wcslen(hint)), &hint_size);
    GetTextExtentPoint32W(dc, ok_text, static_cast<int>(std::wcslen(ok_text)), &ok_size);
    GetTextExtentPoint32W(dc, cancel_text, static_cast<int>(std::wcslen(cancel_text)),
                          &cancel_size);

    const int pad = 16;
    const int gap = 14;
    const int button_pad = 22;
    const int ok_w = std::max(84, static_cast<int>(ok_size.cx) + 2 * button_pad);
    const int cancel_w = std::max(84, static_cast<int>(cancel_size.cx) + 2 * button_pad);
    const int bar_h = std::max(48, static_cast<int>(hint_size.cy) + 2 * pad);

    int bar_w = pad + static_cast<int>(hint_size.cx) + gap + ok_w + gap + cancel_w + pad;
    const int max_w = client.width - 40;
    if (bar_w > max_w) bar_w = max_w;          // the hint elides; the buttons stay

    const int x = (client.width - bar_w) / 2;
    const int y = client.height - bar_h - 36;
    bar_out = RECT{x, y, x + bar_w, y + bar_h};

    HBRUSH bg = CreateSolidBrush(kLabelBg);
    HPEN edge = CreatePen(PS_SOLID, 1, kLabelEdge);
    const HGDIOBJ old_pen = SelectObject(dc, edge);
    const HGDIOBJ old_brush = SelectObject(dc, bg);
    RoundRect(dc, bar_out.left, bar_out.top, bar_out.right, bar_out.bottom, 12, 12);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(bg);
    DeleteObject(edge);

    const int button_h = bar_h - 2 * 10;
    const int button_y = y + 10;
    ok_out = RECT{bar_out.right - pad - cancel_w - gap - ok_w, button_y,
                  bar_out.right - pad - cancel_w - gap, button_y + button_h};
    cancel_out = RECT{bar_out.right - pad - cancel_w, button_y, bar_out.right - pad,
                      button_y + button_h};

    // Confirm carries the accent colour; cancel stays neutral.
    HBRUSH ok_bg = CreateSolidBrush(kAccent);
    HBRUSH cancel_bg = CreateSolidBrush(kLabelEdge);
    if (ok_bg) {
        FillRect(dc, &ok_out, ok_bg);
        DeleteObject(ok_bg);
    }
    if (cancel_bg) {
        FillRect(dc, &cancel_out, cancel_bg);
        DeleteObject(cancel_bg);
    }

    SetTextColor(dc, kLabelFg);
    RECT ok_label = ok_out;
    RECT cancel_label = cancel_out;
    DrawTextW(dc, ok_text, -1, &ok_label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(dc, cancel_text, -1, &cancel_label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // The hint gets whatever is left between the left edge and the buttons.
    RECT hint_rect{bar_out.left + pad, bar_out.top, ok_out.left - gap, bar_out.bottom};
    DrawTextW(dc, hint, -1, &hint_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (old_font) SelectObject(dc, old_font);
    if (font) DeleteObject(font);
}

void draw_readout(HDC dc, const SelectionConfig& sel, const RectPx& origin,
                  SizePx client) noexcept {
    wchar_t text[96] = {};
    const int w = static_cast<int>(width_of(sel.bounds_px));
    const int h = static_cast<int>(height_of(sel.bounds_px));
    // %ls rather than %s: the two conventions disagree on %s in a wide format.
    std::swprintf(text, 96, L"%d x %d  %ls", w, h, localized_shape_label(sel.shape));
    const int len = static_cast<int>(std::wcslen(text));
    if (len <= 0) return;

    HFONT font = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) return;
    const HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kLabelFg);

    SIZE ts{};
    GetTextExtentPoint32W(dc, text, len, &ts);
    const Px panel_w = ts.cx + 2 * kLabelPadXPx;
    const Px panel_h = ts.cy + 2 * kLabelPadYPx;

    const RECT wr = rect_to_client(sel.bounds_px, origin);
    Px x = wr.left;
    Px y = wr.bottom + kLabelGapPx;
    if (y + panel_h > client.height) y = wr.top - kLabelGapPx - panel_h;
    if (y < 0) y = 0;
    if (x + panel_w > client.width) x = client.width - panel_w;
    if (x < 0) x = 0;

    const RECT panel{x, y, x + panel_w, y + panel_h};
    HBRUSH bg = CreateSolidBrush(kLabelBg);
    HBRUSH edge = CreateSolidBrush(kLabelEdge);
    if (bg) FillRect(dc, &panel, bg);
    if (edge) FrameRect(dc, &panel, edge);
    TextOutW(dc, x + kLabelPadXPx, y + kLabelPadYPx, text, len);
    if (bg) DeleteObject(bg);
    if (edge) DeleteObject(edge);

    SelectObject(dc, old_font);
    DeleteObject(font);
}

// Re-derives the gesture from the press point on every move: a press outside
// the shape draws a new region, inside it moves, on a band resizes. Keeping the
// mode derivable means the whole drag needs no extra window state.
void apply_drag(SelectionConfig& draft, const SelectionConfig& start, PointPx anchor,
                PointPx cursor, const RectPx& desktop) noexcept {
    ResizeHandle handle = ResizeHandle::None;
    if (!is_empty(start.bounds_px)) {
        handle = handle_at(start, anchor, kHandleBandPx);
        if (handle == ResizeHandle::None && point_in_shape(start, anchor)) {
            handle = ResizeHandle::Move;
        }
    }

    const Px dx = cursor.x - anchor.x;
    const Px dy = cursor.y - anchor.y;

    if (is_move_handle(handle)) {
        RectPx moved = start.bounds_px;
        moved.left += dx;
        moved.right += dx;
        moved.top += dy;
        moved.bottom += dy;
        draft.bounds_px = clamp_into(moved, desktop);
        return;
    }

    if (handle == ResizeHandle::None) {
        const RectPx r = (start.shape == SelectionShape::Circle)
                             ? square_about_anchor(anchor, cursor)
                             : rect_from_corners(anchor, cursor);
        draft.bounds_px = clamp_edges(r, desktop);
        return;
    }

    RectPx r = start.bounds_px;
    if (handle_has(handle, ResizeHandle::Left)) r.left += dx;
    if (handle_has(handle, ResizeHandle::Right)) r.right += dx;
    if (handle_has(handle, ResizeHandle::Top)) r.top += dy;
    if (handle_has(handle, ResizeHandle::Bottom)) r.bottom += dy;
    r = clamp_edges(r, desktop);
    if (width_of(r) < kMinSelectionPx || height_of(r) < kMinSelectionPx) return;
    draft.bounds_px = r;
}

}  // namespace

SelectionOverlay::SelectionOverlay() = default;

SelectionOverlay::~SelectionOverlay() { destroy(); }

void SelectionOverlay::create(HINSTANCE instance, RectPx virtual_desktop_px, Callbacks callbacks) {
    if (is_empty(virtual_desktop_px)) {
        throw std::invalid_argument("SelectionOverlay::create: empty virtual desktop");
    }
    if (hwnd_) return;

    instance_ = instance;
    callbacks_ = std::move(callbacks);
    virtual_desktop_px_ = virtual_desktop_px;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_DBLCLKS so a double click inside the region can confirm it.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &SelectionOverlay::wnd_proc_thunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);  // system cursor: A/W is irrelevant
    wc.lpszClassName = kOverlayClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("SelectionOverlay::create: RegisterClassExW failed");
    }

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, kOverlayClassName,
                            L"", WS_POPUP, virtual_desktop_px.left, virtual_desktop_px.top,
                            width_of(virtual_desktop_px), height_of(virtual_desktop_px), nullptr,
                            nullptr, instance, this);
    if (!hwnd_) {
        throw std::runtime_error("SelectionOverlay::create: CreateWindowExW failed");
    }

    // Keep the overlay out of its own loupe read-back. Not fatal when the OS
    // refuses: the loupe then simply shows the dim it is drawn over.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
}

void SelectionOverlay::release_backbuffer() noexcept {
    if (mem_dc_) {
        if (mem_bitmap_) {
            SelectObject(mem_dc_, mem_bitmap_old_);
            DeleteObject(mem_bitmap_);
        }
        DeleteDC(mem_dc_);
    }
    mem_dc_ = nullptr;
    mem_bitmap_ = nullptr;
    mem_bitmap_old_ = nullptr;
    mem_bits_ = nullptr;
    buffer_size_ = SizePx{};
}

bool SelectionOverlay::over_hint_button(POINT client_pt, bool& is_ok) const noexcept {
    if (!active_) return false;
    if (PtInRect(&ok_button_, client_pt) != FALSE) {
        is_ok = true;
        return true;
    }
    if (PtInRect(&cancel_button_, client_pt) != FALSE) {
        is_ok = false;
        return true;
    }
    return false;
}

void SelectionOverlay::destroy() noexcept {
    if (hwnd_) {
        KillTimer(hwnd_, kPaintTimerId);
        if (GetCapture() == hwnd_) ReleaseCapture();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    release_backbuffer();
    if (instance_) {
        UnregisterClassW(kOverlayClassName, instance_);
        instance_ = nullptr;
    }
    dragging_ = false;
    active_ = false;
    has_selection_ = false;
}

void SelectionOverlay::open(const SelectionConfig& initial) {
    if (!hwnd_) throw std::logic_error("SelectionOverlay::open: create() was never called");

    draft_ = initial;
    draft_.bounds_px = clamp_into(normalize(draft_.bounds_px), virtual_desktop_px_);
    draft_.corner_radius_px = clamp_corner_radius(draft_.corner_radius_px, draft_.bounds_px);
    start_ = draft_;
    anchor_px_ = PointPx{};
    dragging_ = false;
    has_selection_ = !is_empty(draft_.bounds_px);

    SetWindowPos(hwnd_, HWND_TOPMOST, virtual_desktop_px_.left, virtual_desktop_px_.top,
                 width_of(virtual_desktop_px_), height_of(virtual_desktop_px_),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    rebuild_backbuffer();

    active_ = true;
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    if (callbacks_.on_preview) callbacks_.on_preview(draft_);
    paint();
}

void SelectionOverlay::cancel() {
    if (!active_) return;
    active_ = false;
    dragging_ = false;
    if (hwnd_) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        KillTimer(hwnd_, kPaintTimerId);
        ShowWindow(hwnd_, SW_HIDE);
    }
    // A full-desktop 32-bit buffer is tens of megabytes; holding it while the
    // picker is closed is exactly the kind of idle cost the budget forbids.
    release_backbuffer();
    if (callbacks_.on_cancelled) callbacks_.on_cancelled();
}

void SelectionOverlay::set_virtual_desktop(RectPx rect_px) {
    if (is_empty(rect_px)) {
        throw std::invalid_argument("SelectionOverlay::set_virtual_desktop: empty rect");
    }
    virtual_desktop_px_ = rect_px;
    draft_.bounds_px = clamp_into(draft_.bounds_px, virtual_desktop_px_);
    draft_.corner_radius_px = clamp_corner_radius(draft_.corner_radius_px, draft_.bounds_px);
    has_selection_ = has_selection_ && !is_empty(draft_.bounds_px);
    if (!hwnd_) return;

    SetWindowPos(hwnd_, HWND_TOPMOST, rect_px.left, rect_px.top, width_of(rect_px),
                 height_of(rect_px), SWP_NOACTIVATE | (active_ ? SWP_SHOWWINDOW : 0u));
    rebuild_backbuffer();
    if (active_) paint();
}

void SelectionOverlay::commit() {
    if (!active_) return;
    // Enter on an empty draft would publish a selection with no pixels in it.
    if (is_empty(draft_.bounds_px)) return;
    active_ = false;
    dragging_ = false;
    if (hwnd_) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        KillTimer(hwnd_, kPaintTimerId);
        ShowWindow(hwnd_, SW_HIDE);
    }
    release_backbuffer();
    if (callbacks_.on_committed) callbacks_.on_committed(draft_);
}

void SelectionOverlay::rebuild_backbuffer() {
    if (!hwnd_) return;
    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) return;
    const SizePx size{rc.right - rc.left, rc.bottom - rc.top};
    if (size.width <= 0 || size.height <= 0) return;
    if (size == buffer_size_ && mem_dc_ && mem_bitmap_) return;

    if (mem_dc_) {
        if (mem_bitmap_) {
            SelectObject(mem_dc_, mem_bitmap_old_);
            DeleteObject(mem_bitmap_);
        }
        DeleteDC(mem_dc_);
    }
    mem_dc_ = nullptr;
    mem_bitmap_ = nullptr;
    mem_bitmap_old_ = nullptr;
    mem_bits_ = nullptr;
    buffer_size_ = SizePx{};

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size.width;
    bi.bmiHeader.biHeight = -size.height;  // top-down, matching the client axes
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        return;
    }
    mem_dc_ = dc;
    mem_bitmap_ = bitmap;
    mem_bits_ = bits;
    mem_bitmap_old_ = static_cast<HBITMAP>(SelectObject(mem_dc_, mem_bitmap_));
    buffer_size_ = size;
}

void SelectionOverlay::paint() {
    if (!hwnd_ || !mem_dc_ || !mem_bits_) return;
    if (buffer_size_.width <= 0 || buffer_size_.height <= 0) return;

    fill_dim(static_cast<std::uint32_t*>(mem_bits_), buffer_size_);
    SetBkMode(mem_dc_, TRANSPARENT);
    SetROP2(mem_dc_, R2_COPYPEN);

    const RectPx& origin = virtual_desktop_px_;
    const bool show = has_selection_ && !is_empty(draft_.bounds_px);

    if (show) {
        // Punch the selection out to alpha 0 so the true desktop shows through
        // instead of a "lightened" fake.
        HRGN rgn = make_shape_region(draft_, origin);
        if (rgn) {
            const RECT wr = rect_to_client(visual_bounds(draft_), origin);
            SelectClipRgn(mem_dc_, rgn);
            FillRect(mem_dc_, &wr, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SelectClipRgn(mem_dc_, nullptr);
            DeleteObject(rgn);
        }

        HPEN shadow = CreatePen(PS_SOLID, kOutlineWidthPx + 2, kOutlineShadow);
        if (shadow) {
            stroke_shape(mem_dc_, draft_, origin, shadow);
            DeleteObject(shadow);
        }
        HPEN accent = CreatePen(PS_SOLID, kOutlineWidthPx, kAccent);
        if (accent) {
            stroke_shape(mem_dc_, draft_, origin, accent);
            DeleteObject(accent);
        }
        HBRUSH face = CreateSolidBrush(kHandleFill);
        HBRUSH edge = CreateSolidBrush(kHandleEdge);
        if (face && edge) draw_handles(mem_dc_, draft_.bounds_px, origin, face, edge);
        if (face) DeleteObject(face);
        if (edge) DeleteObject(edge);
    }

    update_loupe();
    if (show) draw_readout(mem_dc_, draft_, origin, buffer_size_);
    draw_hint_bar(mem_dc_, buffer_size_, hint_bar_, ok_button_, cancel_button_);
    fix_alpha(static_cast<std::uint32_t*>(mem_bits_), buffer_size_);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT src{0, 0};
    POINT dst{origin.left, origin.top};
    SIZE size{buffer_size_.width, buffer_size_.height};
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// A pixel loupe so the user can land on an exact source pixel: 8x zoom of the
// area under the cursor, ringed and crosshaired so the centre is unambiguous.
void SelectionOverlay::update_loupe() {
    if (!hwnd_ || !mem_dc_ || !mem_bits_) return;
    if (buffer_size_.width <= 0 || buffer_size_.height <= 0) return;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const RectPx& origin = virtual_desktop_px_;
    const PointPx c{cursor.x - origin.left, cursor.y - origin.top};

    // Never let the loupe cover the pixel it is inspecting.
    Px cx = c.x + kLoupeGapPx + kLoupeRadiusPx;
    Px cy = c.y + kLoupeGapPx + kLoupeRadiusPx;
    if (cx + kLoupeRadiusPx > buffer_size_.width) cx = c.x - kLoupeGapPx - kLoupeRadiusPx;
    if (cy + kLoupeRadiusPx > buffer_size_.height) cy = c.y - kLoupeGapPx - kLoupeRadiusPx;
    cx = std::clamp<Px>(cx, kLoupeRadiusPx,
                        std::max<Px>(kLoupeRadiusPx, buffer_size_.width - kLoupeRadiusPx));
    cy = std::clamp<Px>(cy, kLoupeRadiusPx,
                        std::max<Px>(kLoupeRadiusPx, buffer_size_.height - kLoupeRadiusPx));

    const Px src_half = kLoupeRadiusPx / kLoupeZoom;
    bool blitted = false;
    HDC screen = GetDC(nullptr);
    if (screen) {
        HRGN rgn = CreateEllipticRgn(cx - kLoupeRadiusPx, cy - kLoupeRadiusPx,
                                     cx + kLoupeRadiusPx, cy + kLoupeRadiusPx);
        if (rgn) {
            const int saved = SaveDC(mem_dc_);
            SelectClipRgn(mem_dc_, rgn);
            SetStretchBltMode(mem_dc_, COLORONCOLOR);  // integer zoom: nearest neighbour
            blitted = StretchBlt(mem_dc_, cx - kLoupeRadiusPx, cy - kLoupeRadiusPx,
                                 2 * kLoupeRadiusPx, 2 * kLoupeRadiusPx, screen,
                                 cursor.x - src_half, cursor.y - src_half, 2 * src_half,
                                 2 * src_half, SRCCOPY) != FALSE;
            RestoreDC(mem_dc_, saved);
            DeleteObject(rgn);
        }
        ReleaseDC(nullptr, screen);
    }
    if (blitted) {
        force_opaque_in_ellipse(static_cast<std::uint32_t*>(mem_bits_), buffer_size_, cx, cy,
                                kLoupeRadiusPx, kLoupeRadiusPx);
    }

    HPEN rim = CreatePen(PS_SOLID, 2, kLoupeRim);
    HPEN cross = CreatePen(PS_SOLID, 1, kCrosshair);
    const HGDIOBJ old_brush = SelectObject(mem_dc_, GetStockObject(NULL_BRUSH));
    if (rim) {
        const HGDIOBJ old_pen = SelectObject(mem_dc_, rim);
        Ellipse(mem_dc_, cx - kLoupeRadiusPx, cy - kLoupeRadiusPx, cx + kLoupeRadiusPx,
                cy + kLoupeRadiusPx);
        SelectObject(mem_dc_, old_pen);
    }
    if (cross) {
        const HGDIOBJ old_pen = SelectObject(mem_dc_, cross);
        MoveToEx(mem_dc_, cx - kLoupeRadiusPx, cy, nullptr);
        LineTo(mem_dc_, cx + kLoupeRadiusPx + 1, cy);
        MoveToEx(mem_dc_, cx, cy - kLoupeRadiusPx, nullptr);
        LineTo(mem_dc_, cx, cy + kLoupeRadiusPx + 1);
        SelectObject(mem_dc_, old_pen);
    }
    SelectObject(mem_dc_, old_brush);
    if (rim) DeleteObject(rim);
    if (cross) DeleteObject(cross);
}

void SelectionOverlay::nudge(Px dx, Px dy) {
    if (!active_ || !has_selection_ || is_empty(draft_.bounds_px)) return;
    RectPx moved = draft_.bounds_px;
    moved.left += dx;
    moved.right += dx;
    moved.top += dy;
    moved.bottom += dy;
    moved = clamp_into(moved, virtual_desktop_px_);
    if (moved == draft_.bounds_px) return;
    draft_.bounds_px = moved;
    draft_.corner_radius_px = clamp_corner_radius(draft_.corner_radius_px, draft_.bounds_px);
    if (callbacks_.on_preview) callbacks_.on_preview(draft_);
    paint();
}

void SelectionOverlay::cycle_shape(int direction) {
    if (!active_) return;
    constexpr int kShapeCount = 4;
    const int current = static_cast<int>(draft_.shape);
    const int next = ((current + direction) % kShapeCount + kShapeCount) % kShapeCount;
    if (next == current) return;
    draft_.shape = static_cast<SelectionShape>(next);
    conform_after_shape_change(draft_);
    start_ = draft_;
    has_selection_ = !is_empty(draft_.bounds_px);
    if (callbacks_.on_shape_changed) callbacks_.on_shape_changed(draft_.shape);
    if (callbacks_.on_preview) callbacks_.on_preview(draft_);
    paint();
}

LRESULT CALLBACK SelectionOverlay::wnd_proc_thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<SelectionOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->wnd_proc(hwnd, msg, wp, lp);
}

LRESULT SelectionOverlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                // The cursor states what a press would do, which is the whole
                // difference between "I cannot move this" and "I can".
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                bool over_ok = false;
                if (over_hint_button(pt, over_ok)) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
                const PointPx cursor = point_from_lparam(
                    MAKELPARAM(static_cast<short>(pt.x), static_cast<short>(pt.y)),
                    virtual_desktop_px_);
                const ResizeHandle handle = handle_at(draft_, cursor, kHandleBandPx + 2);
                LPCTSTR cursor_id = IDC_CROSS;
                switch (handle) {
                    case ResizeHandle::Left:
                    case ResizeHandle::Right: cursor_id = IDC_SIZEWE; break;
                    case ResizeHandle::Top:
                    case ResizeHandle::Bottom: cursor_id = IDC_SIZENS; break;
                    case ResizeHandle::TopLeft:
                    case ResizeHandle::BottomRight: cursor_id = IDC_SIZENWSE; break;
                    case ResizeHandle::TopRight:
                    case ResizeHandle::BottomLeft: cursor_id = IDC_SIZENESW; break;
                    case ResizeHandle::Move: cursor_id = IDC_SIZEALL; break;
                    default: break;
                }
                SetCursor(LoadCursor(nullptr, cursor_id));
                return TRUE;
            }
            break;

        case WM_SIZE:
            rebuild_backbuffer();
            if (active_) paint();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (active_) paint();
            return 0;
        }

        case WM_TIMER:
            if (wp == kPaintTimerId) {
                KillTimer(hwnd, kPaintTimerId);
                SetPropW(hwnd, kPaintTickProp, reinterpret_cast<HANDLE>(GetTickCount64()));
                if (active_) paint();
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (dragging_) {
                const PointPx cursor = point_from_lparam(lp, virtual_desktop_px_);
                apply_drag(draft_, start_, anchor_px_, cursor, virtual_desktop_px_);
                draft_.corner_radius_px =
                    clamp_corner_radius(draft_.corner_radius_px, draft_.bounds_px);
                if (callbacks_.on_preview) callbacks_.on_preview(draft_);
            }
            // Leading-edge throttle: the first move of a burst paints at once
            // and the trailing timer catches the final position.
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG last =
                static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(GetPropW(hwnd, kPaintTickProp)));
            if (last == 0 || now - last >= kRepaintIntervalMs) {
                SetPropW(hwnd, kPaintTickProp, reinterpret_cast<HANDLE>(now));
                KillTimer(hwnd, kPaintTimerId);
                if (active_) paint();
            } else {
                SetTimer(hwnd, kPaintTimerId,
                         static_cast<UINT>(kRepaintIntervalMs - (now - last)), nullptr);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
            if (active_) {
                // point_from_lparam yields the domain point type, not POINT.
                const PointPx hit = point_from_lparam(lp, virtual_desktop_px_);
                const POINT pt{hit.x, hit.y};
                bool over_ok = false;
                if (over_hint_button(pt, over_ok)) {
                    // Confirming with the mouse does not depend on this window
                    // holding keyboard focus, which SetForegroundWindow is not
                    // always granted.
                    if (over_ok) {
                        commit();
                    } else {
                        cancel();
                    }
                    return 0;
                }
                anchor_px_ = point_from_lparam(lp, virtual_desktop_px_);
                start_ = draft_;
                dragging_ = true;
                SetCapture(hwnd);
            }
            return 0;

        case WM_LBUTTONDBLCLK:
            // A double click inside the region is the other obvious way to say
            // "this one".
            if (active_) {
                const PointPx cursor = point_from_lparam(lp, virtual_desktop_px_);
                if (point_in_shape(draft_, cursor)) commit();
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                // A click that dragged out nothing keeps the previous region.
                if (is_empty(draft_.bounds_px)) {
                    draft_ = start_;
                    draft_.bounds_px = clamp_into(draft_.bounds_px, virtual_desktop_px_);
                    has_selection_ = !is_empty(draft_.bounds_px);
                } else {
                    has_selection_ = true;
                }
                if (callbacks_.on_preview) callbacks_.on_preview(draft_);
                paint();
            }
            return 0;

        case WM_KEYDOWN: {
            const Px step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            switch (wp) {
                case VK_LEFT:
                    nudge(-step, 0);
                    return 0;
                case VK_RIGHT:
                    nudge(step, 0);
                    return 0;
                case VK_UP:
                    nudge(0, -step);
                    return 0;
                case VK_DOWN:
                    nudge(0, step);
                    return 0;
                case VK_TAB:
                    cycle_shape((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                    return 0;
                case VK_RETURN:
                    commit();
                    return 0;
                case VK_ESCAPE:
                    cancel();
                    return 0;
                case '1':
                case '2':
                case '3':
                case '4': {
                    const int target = static_cast<int>(wp - '1');
                    const int current = static_cast<int>(draft_.shape);
                    cycle_shape(((target - current) % 4 + 4) % 4);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace mag
