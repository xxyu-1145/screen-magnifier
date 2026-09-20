// app/control_window.cpp — the settings and status window.
//
// There is no resource compiler in this toolchain, so every control is created
// in code and the window paints its own chrome. The painting goes through GDI+
// (see ui_draw.h) rather than GDI's RoundRect, because a flat fill behind a
// one-pixel pen reads as a wireframe: anti-aliased paths, gradients and soft
// shadows are what make a surface look rendered.
//
// Labels are real static controls, so their text goes through the same text
// stack as the rest of the system, and every string comes from core/i18n.h.

#include "app/control_window.h"

#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

#include "app/ui_draw.h"
#include "core/i18n.h"
#include "platform/app_icon.h"

namespace mag {
namespace {

using namespace Gdiplus;
// The drawing primitives live in mag::ui; the enclosing namespace here is
// mag, so bring them in by name rather than qualifying every call.
using namespace ui;

constexpr wchar_t kWndClass[] = L"MagControlWindow";

// Control ids.
//   100   plain static caption
//   200   language toggles        (kIdLangFirst + Language)
//   1000  shape toggles           (kIdShapeFirst + index into kShapeOrder)
//   1010  factor slider           1012 factor read-out
//   1020  factor presets          (kIdPresetFirst + preset index)
//   1030  output width            1031 output height      1032 apply
//   1033  fit to source           1034 keep aspect
//   1100  hotkey chord edits      (kIdHotkeyFirst + HotkeyAction)
//   1200  pass-through            1201 strict compat      1202 edge dwell on
//   1203  edge dwell ms
//   1210  filter toggles          (kIdFilterFirst + ScaleFilter)
//   1220  exclude capture         1221 show border
//   1300  pick region             1301 start/stop         1302 save   1303 quit
//   1304  restore defaults
//   1400  status line
enum : int {
    kIdLabel = 100,
    kIdLangFirst = 200,
    kIdShapeFirst = 1000,
    kIdFactorSlider = 1010,
    kIdFactorLabel = 1012,
    kIdPresetFirst = 1020,
    kIdPresetEditFirst = 1024,
    kIdOutputWidth = 1030,
    kIdOutputHeight = 1031,
    kIdOutputApply = 1032,
    kIdOutputFit = 1033,
    kIdKeepAspect = 1034,
    kIdSizeSlider = 1035,
    kIdSelectionSlotFirst = 1050,
    kIdSaveSelection = 1054,
    kIdHotkeyFirst = 1100,
    kIdPassThrough = 1200,
    kIdStrictCompat = 1201,
    kIdEdgeDwellEnable = 1202,
    kIdEdgeDwellMs = 1203,
    kIdFilterFirst = 1210,
    kIdExcludeCapture = 1220,
    kIdShowBorder = 1221,
    kIdPickRegion = 1300,
    kIdStartStop = 1301,
    kIdSaveSettings = 1302,
    kIdQuit = 1303,
    kIdRestoreDefaults = 1304,
    kIdStatusLine = 1400,
};

// The slider works in hundredths of the factor: 100 == 1.00x, and the ceiling
// follows kFactorMax so the slider can never offer a factor set_factor() would
// reject.
constexpr int kSliderMin = 100;
constexpr int kSliderMax = static_cast<int>(kFactorMax / (kQ16One / 100));

// Messages between the slider control and this window.
constexpr UINT kMsgSliderSet = WM_APP + 11;    // wparam = position
constexpr UINT kMsgSliderMoved = WM_APP + 12;  // wparam = position
// The two sliders do not share a range: the factor runs in hundredths of 1.00x,
// the window size runs in output pixels and its ceiling is the desktop, which is
// only known once there is a window to ask. The parent owns the range, so the
// control asks for it rather than carrying a copy that can go stale.
constexpr UINT kMsgSliderRange = WM_APP + 13;  // lparam = slider, returns MAKELONG(lo, hi)
constexpr UINT kMsgSizeMoved = WM_APP + 14;    // wparam = output width in pixels

// Fires shortly after typing stops in the factor box, so a number takes effect
// without the user having to press anything.
constexpr UINT_PTR kFactorCommitTimer = 0x51;
constexpr UINT kFactorCommitDelayMs = 500;

// Saving a region writes it into whichever numbered button is lit, and that
// button does not change when the region does -- so saving twice in a row
// overwrites the same slot and the screen shows nothing whatsoever, which reads
// as the button being broken. It says so on itself for a moment instead.
constexpr UINT_PTR kSaveConfirmTimer = 0x52;
constexpr UINT kSaveConfirmMs = 1400;

constexpr SelectionShape kShapeOrder[4] = {
    SelectionShape::Rectangle, SelectionShape::Circle, SelectionShape::Ellipse,
    SelectionShape::RoundedRectangle};

const ScaleFilter kFilterOrder[4] = {ScaleFilter::Auto, ScaleFilter::Point,
                                     ScaleFilter::Bilinear, ScaleFilter::Bicubic};

enum class ButtonKind { Primary, Secondary, Segment, Checkbox };

int scaled(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

UINT dpi_for(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return (dpi >= 48 && dpi <= 768) ? dpi : 96u;
}

void set_text(HWND h, const wchar_t* text) {
    if (h && text) SetWindowTextW(h, text);
}

void set_text_int(HWND h, long long value) {
    if (!h) return;
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%lld", value);
    SetWindowTextW(h, buf);
}

std::wstring format_factor_text(Q16 q) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%.2fx", q16_to_double(q));
    return buf;
}

int slider_pos_from_q16(Q16 q) {
    const unsigned long long v = (static_cast<unsigned long long>(q) * 100ull + 32768ull) / kQ16One;
    return static_cast<int>(std::clamp<unsigned long long>(v, kSliderMin, kSliderMax));
}

// Reads what someone would actually type into a factor box: "7", "7.5", "7.5x",
// with either a half-width or a full-width decimal point. Returns false for
// anything else, and clamps a number that is out of range rather than refusing
// it, so typing "99" lands on the ceiling instead of doing nothing.
bool parse_factor_text(const wchar_t* text, Q16& out) {
    if (text == nullptr) return false;
    std::wstring digits;
    for (const wchar_t c : std::wstring(text)) {
        if (c == L'x' || c == L'X' || c == L' ' || c == L'\t' || c == L'+') continue;
        if (c == L',' || c == L'，') {
            digits.push_back(L'.');
            continue;
        }
        digits.push_back(c);
    }
    if (digits.empty()) return false;

    wchar_t* end = nullptr;
    const double typed = std::wcstod(digits.c_str(), &end);
    if (end == digits.c_str() || end == nullptr || *end != L'\0') return false;
    if (!std::isfinite(typed) || typed <= 0.0) return false;

    const double lo = static_cast<double>(kFactorMin) / static_cast<double>(kQ16One);
    const double hi = static_cast<double>(kFactorMax) / static_cast<double>(kQ16One);
    const auto rounded =
        static_cast<Q16>(std::clamp(typed, lo, hi) * static_cast<double>(kQ16One) + 0.5);
    out = rounded < kFactorMin ? kFactorMin : (rounded > kFactorMax ? kFactorMax : rounded);
    return true;
}

Q16 q16_from_slider_pos(int pos) {
    const int clamped = std::clamp(pos, kSliderMin, kSliderMax);
    return static_cast<Q16>((static_cast<unsigned long long>(clamped) * kQ16One + 50ull) / 100ull);
}

// The keys a chord is *made of* rather than the key it ends on. The message for
// Ctrl arrives with Ctrl already down, so committing on it produced a
// "Ctrl+VK_11" binding and ended the capture before the letter was reached.
bool is_modifier_key(UINT vk) noexcept {
    switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT:
        case VK_LCONTROL: case VK_RCONTROL:
        case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        // What an input method sends for a keystroke it has taken for itself;
        // it carries no key of its own.
        case VK_PROCESSKEY:
            return true;
        default:
            return false;
    }
}

bool face_available(HDC dc, const wchar_t* face) {
    HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, face);
    if (!font) return false;
    const HGDIOBJ old = SelectObject(dc, font);
    wchar_t actual[LF_FACESIZE] = {};
    const int n = GetTextFaceW(dc, LF_FACESIZE, actual);
    SelectObject(dc, old);
    DeleteObject(font);
    return n > 0 && _wcsicmp(actual, face) == 0;
}

// Fonts for the controls that are still real Windows controls -- the edit
// fields. Everything owner-drawn is painted through GDI+ instead.
HFONT make_font(UINT dpi, int point_size, int weight) {
    static const wchar_t* const kCandidates[] = {L"Microsoft YaHei UI", L"Microsoft YaHei",
                                                 L"Segoe UI", L"Tahoma"};
    HDC dc = GetDC(nullptr);
    const wchar_t* face = nullptr;
    if (dc) {
        for (const wchar_t* candidate : kCandidates) {
            if (face_available(dc, candidate)) {
                face = candidate;
                break;
            }
        }
        ReleaseDC(nullptr, dc);
    }
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(point_size, static_cast<int>(dpi), 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    if (face) std::wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

// ===========================================================================
// The slider
// ===========================================================================
//
// A STATIC control that paints and drives itself. The stock trackbar cannot be
// restyled beyond its channel colour and draws its thumb from the theme, so it
// would be the one control that did not match.

int slider_position(HWND slider) {
    return static_cast<int>(GetWindowLongPtrW(slider, GWLP_USERDATA));
}

struct SliderRange {
    int lo;
    int hi;
};

SliderRange slider_range(HWND slider) {
    const LRESULT packed = SendMessageW(GetParent(slider), kMsgSliderRange, 0,
                                        reinterpret_cast<LPARAM>(slider));
    return SliderRange{LOWORD(packed), HIWORD(packed)};
}

UINT slider_moved_message(HWND slider) {
    return GetDlgCtrlID(slider) == kIdSizeSlider ? kMsgSizeMoved : kMsgSliderMoved;
}

void slider_paint(HWND slider) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(slider, &ps);
    RECT rc{};
    GetClientRect(slider, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) {
        EndPaint(slider, &ps);
        return;
    }

    const int pos = slider_position(slider);
    const SliderRange range = slider_range(slider);
    const int span = std::max(1, range.hi - range.lo);
    const float t = static_cast<float>(pos - range.lo) / static_cast<float>(span);
    const float thumb_r = h * 0.32f;
    const float track_h = std::max(3.0f, h * 0.11f);
    const float track_y = h * 0.5f - track_h * 0.5f;
    const float track_x0 = thumb_r;
    const float track_x1 = static_cast<float>(w) - thumb_r;
    const float thumb_x = track_x0 + (track_x1 - track_x0) * t;

    Bitmap buffer(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&buffer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const Theme& th = theme();
        // The bitmap starts fully transparent, and a transparent pixel blitted
        // onto a plain window DC comes out black; the card colour goes down
        // first so the control matches the surface it sits on.
        SolidBrush backdrop(th.card);
        g.FillRectangle(&backdrop, 0, 0, w, h);
        fill_round(g, Gdiplus::RectF(track_x0, track_y, track_x1 - track_x0, track_h),
                   track_h * 0.5f, Gdiplus::Color(255, 0xDB, 0xE0, 0xEA));
        if (thumb_x - track_x0 > 0.5f) {
            fill_round_vertical(g, Gdiplus::RectF(track_x0, track_y, thumb_x - track_x0, track_h),
                                track_h * 0.5f, th.accent,
                                Gdiplus::Color(255, 0x2E, 0x66, 0xD0));
        }

        const Gdiplus::RectF knob{thumb_x - thumb_r, h * 0.5f - thumb_r, thumb_r * 2.0f,
                                  thumb_r * 2.0f};
        // A shadow under the knob, then the knob: white body, accent ring.
        for (int i = 4; i >= 1; --i) {
            const float grow = static_cast<float>(i) * 1.0f;
            GraphicsPath ring;
            ring.AddEllipse(knob.X - grow, knob.Y - grow + 1.2f, knob.Width + 2 * grow,
                            knob.Height + 2 * grow);
            SolidBrush shadow(Gdiplus::Color(22, 0x1B, 0x24, 0x38));
            g.FillPath(&shadow, &ring);
        }
        GraphicsPath knob_path;
        knob_path.AddEllipse(knob);
        SolidBrush body(Gdiplus::Color(255, 0xFF, 0xFF, 0xFF));
        g.FillPath(&body, &knob_path);
        Pen ring_pen(th.accent, std::max(1.6f, h * 0.075f));
        g.DrawPath(&ring_pen, &knob_path);
    }

    Graphics screen(dc);
    screen.DrawImage(&buffer, 0, 0, w, h);
    EndPaint(slider, &ps);
}

void slider_set_from_x(HWND slider, int x) {
    RECT rc{};
    GetClientRect(slider, &rc);
    const float thumb_r = rc.bottom * 0.32f;
    const float x0 = thumb_r;
    const float x1 = static_cast<float>(rc.right) - thumb_r;
    if (x1 <= x0) return;
    const float t = std::clamp((static_cast<float>(x) - x0) / (x1 - x0), 0.0f, 1.0f);
    const SliderRange range = slider_range(slider);
    const int pos = range.lo + static_cast<int>((range.hi - range.lo) * t + 0.5f);
    if (pos == slider_position(slider)) return;
    SetWindowLongPtrW(slider, GWLP_USERDATA, pos);
    InvalidateRect(slider, nullptr, FALSE);
    SendMessageW(GetParent(slider), slider_moved_message(slider), static_cast<WPARAM>(pos), 0);
}

LRESULT CALLBACK slider_subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            slider_paint(hwnd);
            return 0;
        case kMsgSliderSet:
            if (static_cast<int>(wparam) != slider_position(hwnd)) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(wparam));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            slider_set_from_x(hwnd, GET_X_LPARAM(lparam));
            return 0;
        case WM_MOUSEMOVE:
            if (GetCapture() == hwnd) slider_set_from_x(hwnd, GET_X_LPARAM(lparam));
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));  // IDC_HAND
            return TRUE;
        case WM_KILLFOCUS:
            if (GetCapture() == hwnd) ReleaseCapture();
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

// ===========================================================================
// Owner-drawn buttons
// ===========================================================================

// The role and the checked state travel with the control as window properties,
// so the painter needs no access to the window's internals.
void button_set_role(HWND button, ButtonKind kind) {
    SetPropW(button, L"MagKind", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(kind)));
}

ButtonKind button_role(HWND button) {
    const HANDLE p = GetPropW(button, L"MagKind");
    return p ? static_cast<ButtonKind>(reinterpret_cast<INT_PTR>(p)) : ButtonKind::Secondary;
}

bool button_checked(HWND button) {
    return GetPropW(button, L"MagChecked") != nullptr;
}

void button_set_checked(HWND button, bool checked) {
    if (!button) return;
    if (button_checked(button) == checked) return;   // nothing to repaint
    SetPropW(button, L"MagChecked", checked ? reinterpret_cast<HANDLE>(1) : nullptr);
    InvalidateRect(button, nullptr, FALSE);
}

// Rounded push buttons, segmented toggles and checkboxes, all from one painter.
//
// `backdrop` is the colour the surface under the control is painted with. The
// control composes into a bitmap and blits it whole, and a bitmap that starts
// transparent leaves whatever was underneath it alone -- which is how switching
// the language came to draw the English label on top of the Chinese one: the
// checkbox only paints a box and some text, so the rest of its rectangle was
// still the previous language. Filling with the colour it is actually sitting
// on also means the anti-aliased edges of a rounded button blend into the card
// or the page rather than into whatever the window last showed there.
void paint_button(const DRAWITEMSTRUCT* dis, ButtonKind kind, bool checked, bool hovered,
                  const Gdiplus::Color& backdrop) {
    const Theme& th = theme();
    const int width = dis->rcItem.right - dis->rcItem.left;
    const int height = dis->rcItem.bottom - dis->rcItem.top;
    if (width <= 0 || height <= 0) return;

    const Gdiplus::RectF box(0.5f, 0.5f, static_cast<float>(width) - 1.0f,
                             static_cast<float>(height) - 1.0f);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    wchar_t label[192] = {};
    GetWindowTextW(dis->hwndItem, label, 192);

    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    {
        Graphics g(&buffer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // AntiAliasGridFit, not ClearType: these bitmaps carry alpha, and
        // ClearType's subpixel filtering fringes glyph edges on a transparent
        // surface, which reads as text that is too heavy.
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        SolidBrush under(backdrop);
        g.FillRectangle(&under, 0, 0, width, height);

        const float radius = std::min(11.0f, box.Height * 0.30f);

        if (kind == ButtonKind::Checkbox) {
            // A drawn box on the left, the caption beside it.
            const float side = std::min(box.Height - 4.0f, 20.0f);
            const Gdiplus::RectF mark{box.X, box.Y + (box.Height - side) * 0.5f, side, side};
            const float r = side * 0.30f;
            const float d = r * 2.0f;
            GraphicsPath path;
            path.AddArc(mark.X, mark.Y, d, d, 180.0f, 90.0f);
            path.AddArc(mark.GetRight() - d, mark.Y, d, d, 270.0f, 90.0f);
            path.AddArc(mark.GetRight() - d, mark.GetBottom() - d, d, d, 0.0f, 90.0f);
            path.AddArc(mark.X, mark.GetBottom() - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();

            if (checked) {
                fill_round_vertical(g, mark, r, pressed ? th.accent_dark : th.accent,
                                    pressed ? th.accent_dark : th.accent);
                draw_check(g, mark, Gdiplus::Color(255, 0xFF, 0xFF, 0xFF),
                           std::max(1.7f, side * 0.15f));
            } else {
                SolidBrush fill(hovered ? Gdiplus::Color(255, 0xF3, 0xF6, 0xFB) : th.field);
                g.FillPath(&fill, &path);
                Pen edge(hovered ? th.accent : th.field_edge, 1.4f);
                g.DrawPath(&edge, &path);
            }

            Font font(th.body_family, th.body_size, FontStyleRegular, UnitPoint);
            const Gdiplus::RectF text_box{mark.GetRight() + 9.0f, box.Y,
                                          box.GetRight() - mark.GetRight() - 9.0f, box.Height};
            draw_text(g, label, text_box, font, disabled ? th.text_muted : th.text,
                      TextLeft | TextVCenter | TextEllipsis);
        } else if (kind == ButtonKind::Primary) {
            Gdiplus::RectF shadow = box;
            shadow.Offset(0.0f, 1.0f);
            ui::drop_shadow(g, shadow, radius, 3.5f, 96);
            const Gdiplus::Color top =
                pressed ? th.accent_dark : Gdiplus::Color(255, 0x5C, 0x98, 0xF5);
            const Gdiplus::Color bottom = pressed ? th.accent_dark : th.accent;
            fill_round_vertical(g, box, radius, top, bottom);
            // A lighter line just inside the top edge is what gives a flat
            // button its sense of being lit.
            GraphicsPath inner;
            round_rect_path(inner,
                            Gdiplus::RectF(box.X + 1.0f, box.Y + 1.0f, box.Width - 2.0f,
                                           box.Height - 2.0f),
                            std::max(1.0f, radius - 1.0f));
            Pen gloss(Gdiplus::Color(58, 0xFF, 0xFF, 0xFF), 1.0f);
            g.DrawPath(&gloss, &inner);

            Font font(th.body_family, th.body_size, FontStyleBold, UnitPoint);
            draw_text(g, label, box, font, th.on_accent, TextCenter | TextVCenter | TextEllipsis);
        } else {
            if (checked) {
                fill_round_vertical(g, box, radius, pressed ? th.accent_dark : th.accent,
                                    pressed ? th.accent_dark : Gdiplus::Color(255, 0x2E, 0x66, 0xD0));
                Pen gloss(Gdiplus::Color(52, 0xFF, 0xFF, 0xFF), 1.0f);
                GraphicsPath inner;
                round_rect_path(inner,
                                Gdiplus::RectF(box.X + 1.0f, box.Y + 1.0f, box.Width - 2.0f,
                                               box.Height - 2.0f),
                                std::max(1.0f, radius - 1.0f));
                g.DrawPath(&gloss, &inner);
            } else {
                const Gdiplus::Color top =
                    hovered ? Gdiplus::Color(255, 0xFF, 0xFF, 0xFF) : th.control;
                const Gdiplus::Color bottom =
                    hovered ? Gdiplus::Color(255, 0xF3, 0xF7, 0xFD) : th.control;
                fill_round_vertical(g, box, radius, top, bottom);
                GraphicsPath path;
                round_rect_path(path,
                                Gdiplus::RectF(box.X + 0.65f, box.Y + 0.65f, box.Width - 1.3f,
                                               box.Height - 1.3f),
                                std::max(1.0f, radius - 0.65f));
                Pen edge(hovered ? th.accent : th.control_edge, 1.3f);
                g.DrawPath(&edge, &path);
            }

            Font font(th.body_family, th.body_size,
                      checked ? FontStyleBold : FontStyleRegular, UnitPoint);
            draw_text(g, label, box, font, checked ? th.on_accent : th.text,
                      TextCenter | TextVCenter | TextEllipsis);
        }
    }

    Graphics screen(dis->hDC);
    screen.DrawImage(&buffer, static_cast<int>(dis->rcItem.left),
                     static_cast<int>(dis->rcItem.top));
}

// ===========================================================================
// Subclasses
// ===========================================================================

LRESULT CALLBACK button_subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<ControlWindow*>(ref);
    switch (message) {
        case WM_MOUSEMOVE:
            if (self) {
                const HWND previous = self->hovered();
                if (previous != hwnd) {
                    self->set_hovered(hwnd);
                    if (previous) InvalidateRect(previous, nullptr, FALSE);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
            }
            break;
        case WM_MOUSELEAVE:
            if (self && self->hovered() == hwnd) {
                self->set_hovered(nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_LBUTTONDOWN:
            // Clicking anything ends a pending chord capture, so it can never
            // latch and swallow every later keystroke.
            if (self) self->cancel_chord_capture();
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK chord_edit_subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<ControlWindow*>(ref);
    if (!self) return DefSubclassProc(hwnd, message, wparam, lparam);
    switch (message) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_LBUTTONDOWN:
            self->begin_chord_capture(hwnd);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            self->handle_chord_key(static_cast<UINT>(wparam));
            return 0;
        // The mouse buttons cannot be registered with the system at all, so
        // they are bound the same way they are pressed: while the field is
        // armed and the cursor is over it. The left button is deliberately not
        // among them -- clicking the field is what arms it.
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            self->handle_chord_mouse_button(message, wparam);
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        case WM_KILLFOCUS:
            // Only the field that is actually armed may end the capture: focus
            // often moves *to* another chord field, and cancelling on behalf of
            // the one being left behind would disarm the one being entered.
            if (self->is_armed_field(hwnd)) self->cancel_chord_capture();
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

struct FontPass {
    HFONT font;
};

Gdiplus::Color mix_colour(const Gdiplus::Color& a, const Gdiplus::Color& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto mix = [t](BYTE x, BYTE y) {
        return static_cast<BYTE>(static_cast<float>(x) +
                                 (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f);
    };
    return Gdiplus::Color(255, mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()),
                          mix(a.GetB(), b.GetB()));
}

// What the parent paints on the surface underneath a control: flat white inside
// a card or the header row, and the page's own vertical gradient everywhere
// else. An owner-drawn control that composes into a bitmap has to fill its
// rectangle with this before it draws, or the bitmap's untouched pixels blit as
// "leave what was there" and the previous contents stay on screen.
Gdiplus::Color backdrop_under(HWND hwnd, const RECT* cards, int card_count, const RECT& header,
                              const RECT& item) {
    const Theme& th = theme();
    for (int i = 0; i < card_count; ++i) {
        const RECT& c = cards[i];
        if (item.left >= c.left && item.right <= c.right && item.top >= c.top &&
            item.bottom <= c.bottom) {
            return th.card;
        }
    }
    if (item.bottom <= header.bottom) return Gdiplus::Color(255, 0xFF, 0xFF, 0xFF);

    RECT client{};
    GetClientRect(hwnd, &client);
    if (client.bottom <= 0) return th.page_top;
    const float middle = static_cast<float>(item.top + item.bottom) * 0.5f;
    return mix_colour(th.page_top, th.page_bottom,
                      middle / static_cast<float>(client.bottom));
}

BOOL CALLBACK apply_font_to_child(HWND child, LPARAM param) {
    const auto* pass = reinterpret_cast<const FontPass*>(param);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(pass->font), TRUE);
    return TRUE;
}

}  // namespace

// ===========================================================================
// Impl
// ===========================================================================

struct ControlWindow::Impl {
    HWND title{nullptr};
    HWND lang[2]{};

    HWND zoom_caption{nullptr};
    HWND zoom_value{nullptr};
    HWND slider{nullptr};
    HWND preset[kPresetCount]{};
    // One editable factor under each preset button: the button jumps there, the
    // field says what "there" is.
    HWND preset_edit[kPresetCount]{};

    HWND shape_caption{nullptr};
    HWND shape[4]{};
    // Keep-region buttons and the button that stores the current one.
    HWND slot[kSelectionSlotCount]{};
    HWND save_selection{nullptr};

    HWND out_caption{nullptr};
    HWND out_w_label{nullptr};
    HWND out_h_label{nullptr};
    HWND out_w{nullptr};
    HWND out_h{nullptr};
    HWND out_apply{nullptr};
    HWND out_fit{nullptr};
    HWND out_keep{nullptr};
    // Drags the window size, keeping the window's middle where it is.
    HWND out_size_slider{nullptr};

    HWND hk_caption{nullptr};
    HWND hk_caption2{nullptr};
    HWND hk_label[kHotkeyCount]{};
    HWND hk_edit[kHotkeyCount]{};

    HWND act_caption{nullptr};
    HWND pass_through{nullptr};
    HWND strict{nullptr};
    HWND dwell_check{nullptr};
    HWND dwell_ms{nullptr};
    HWND dwell_unit{nullptr};

    HWND rnd_caption{nullptr};
    HWND filter_caption{nullptr};
    HWND filter[4]{};
    HWND exclude{nullptr};
    HWND border{nullptr};

    HWND status_text{nullptr};
    HWND pick{nullptr};
    HWND startstop{nullptr};
    HWND save{nullptr};
    HWND quit{nullptr};
    HWND restore{nullptr};

    HFONT body_font{nullptr};
    HFONT title_font{nullptr};
    HICON icon_large{nullptr};
    HICON icon_small{nullptr};

    RECT cards[8]{};
    int card_count{0};
    RECT header{};
    int content_height{0};
    int content_width{0};
    // The smallest frame the current layout still fits in, recomputed by every
    // layout and enforced through WM_GETMINMAXINFO so a resize can never clip a
    // control out of reach.
    int min_track_w{0};
    int min_track_h{0};
    // True while the factor box owns the keyboard. The window syncs several
    // times a second, which would otherwise overwrite the number as it is typed.
    bool factor_editing{false};
    // True while this window is the one writing the factor box, so its own
    // writes are not mistaken for the user typing.
    bool writing_factor{false};
    // The size slider works in output pixels, so it needs the window's current
    // size to scale from and the desktop's to stop at.
    SizePx last_output{};
    SizePx desktop{};
    // Which preset field owns the keyboard, or -1. Only one can be edited at a
    // time, so the periodic sync has to leave exactly that one alone.
    int preset_editing{-1};

    HWND hover{nullptr};
    bool have_synced{false};
    int last_out_w{-1};
    int last_out_h{-1};
    InteractionState last_state{InteractionState::Off};
    std::unordered_map<int, bool> checks;

    bool check_of(int id) const {
        const auto it = checks.find(id);
        return it != checks.end() && it->second;
    }
};

// ===========================================================================
// Construction
// ===========================================================================

ControlWindow::ControlWindow() : impl_(new Impl()) {}

ControlWindow::~ControlWindow() {
    destroy();
    delete impl_;
}

LRESULT CALLBACK ControlWindow::wnd_proc_thunk(HWND hwnd, UINT message, WPARAM wparam,
                                               LPARAM lparam) {
    ControlWindow* self = reinterpret_cast<ControlWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<ControlWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    }
    if (self) return self->wnd_proc(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ControlWindow::create(HINSTANCE instance, Callbacks callbacks,
                           const std::function<void()>& on_closed) {
    instance_ = instance;
    callbacks_ = std::move(callbacks);
    on_closed_ = on_closed;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ControlWindow::wnd_proc_thunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hbrBackground = nullptr;  // the window paints its own page
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "RegisterClassExW failed for the settings window");
    }

    // No WS_EX_COMPOSITED: it makes the system recomposite the whole window and
    // every child on any invalidation, which measured at ~8% of a core just for
    // having the window on screen. The window already double-buffers its own
    // painting and clips children, which is what actually stops the flicker.
    hwnd_ = CreateWindowExW(0, kWndClass, tr(Str::AppTitle),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 900, 620, nullptr, nullptr, instance,
                            this);
    if (!hwnd_) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateWindowExW failed for the settings window");
    }

    // The icon is drawn rather than loaded from a resource, because there is no
    // resource compiler here to embed one with.
    impl_->icon_large = make_app_icon(GetSystemMetrics(SM_CXICON));
    impl_->icon_small = make_app_icon(GetSystemMetrics(SM_CXSMICON));
    if (impl_->icon_large) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(impl_->icon_large));
    }
    if (impl_->icon_small) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(impl_->icon_small));
    }

    build_controls();
    apply_font();
    layout_controls(0, 0, true);
}

void ControlWindow::destroy() noexcept {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (impl_) {
        if (impl_->body_font) DeleteObject(impl_->body_font);
        if (impl_->title_font) DeleteObject(impl_->title_font);
        if (impl_->icon_large) DestroyIcon(impl_->icon_large);
        if (impl_->icon_small) DestroyIcon(impl_->icon_small);
        impl_->body_font = nullptr;
        impl_->title_font = nullptr;
        impl_->icon_large = nullptr;
        impl_->icon_small = nullptr;
        impl_->hover = nullptr;
    }
    if (instance_) {
        UnregisterClassW(kWndClass, instance_);
        instance_ = nullptr;
    }
}

void ControlWindow::build_controls() {
    auto& s = *impl_;
    const DWORD base = SS_LEFT | SS_CENTERIMAGE;
    const DWORD edit = ES_LEFT | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    const DWORD edit_ex = WS_EX_CLIENTEDGE;

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_,
                               nullptr);
    };
    auto ownerdraw = [&](const wchar_t* text, int id, ButtonKind kind) {
        HWND h = make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, 0, id);
        button_set_role(h, kind);
        return h;
    };

    s.title = make(L"STATIC", tr(Str::AppTitle), base, 0, kIdLabel);
    s.lang[0] = ownerdraw(tr(Str::LanguageChinese), kIdLangFirst, ButtonKind::Segment);
    s.lang[1] = ownerdraw(tr(Str::LanguageEnglish), kIdLangFirst + 1, ButtonKind::Segment);

    // --- magnification ---
    s.zoom_caption = make(L"STATIC", tr(Str::SectionZoom), base, 0, kIdLabel);
    // Editable as well as slidable: the slider is fine for a nudge, but typing
    // the number is how anyone reaches the factor they actually have in mind.
    // It keeps the read-out's id and shows the same "4.00x" text.
    s.zoom_value = make(L"EDIT", L"4.00x", edit | ES_RIGHT, edit_ex, kIdFactorLabel);
    s.slider = make(L"STATIC", L"", SS_NOTIFY, 0, kIdFactorSlider);
    SetWindowLongPtrW(s.slider, GWLP_USERDATA, kSliderMin);
    SetWindowSubclass(s.slider, &slider_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
    for (int i = 0; i < kPresetCount; ++i) {
        s.preset[i] = ownerdraw(L"", kIdPresetFirst + i, ButtonKind::Segment);
        s.preset_edit[i] = make(L"EDIT", L"", edit | ES_RIGHT | ES_CENTER, edit_ex,
                                kIdPresetEditFirst + i);
    }

    // --- shape ---
    s.shape_caption = make(L"STATIC", tr(Str::SectionShape), base, 0, kIdLabel);
    for (int i = 0; i < 4; ++i) {
        s.shape[i] = ownerdraw(tr(shape_label(kShapeOrder[i])), kIdShapeFirst + i,
                               ButtonKind::Segment);
    }
    for (int i = 0; i < kSelectionSlotCount; ++i) {
        wchar_t label[8];
        std::swprintf(label, 8, L"%d", i + 1);
        s.slot[i] = ownerdraw(label, kIdSelectionSlotFirst + i, ButtonKind::Segment);
    }
    s.save_selection = ownerdraw(tr(Str::SaveSelection), kIdSaveSelection, ButtonKind::Secondary);

    // --- output window ---
    s.out_caption = make(L"STATIC", tr(Str::SectionWindow), base, 0, kIdLabel);
    s.out_w_label = make(L"STATIC", tr(Str::Width), base, 0, kIdLabel);
    s.out_h_label = make(L"STATIC", tr(Str::Height), base, 0, kIdLabel);
    s.out_w = make(L"EDIT", L"640", edit | ES_NUMBER, edit_ex, kIdOutputWidth);
    s.out_h = make(L"EDIT", L"480", edit | ES_NUMBER, edit_ex, kIdOutputHeight);
    s.out_apply = ownerdraw(tr(Str::Apply), kIdOutputApply, ButtonKind::Secondary);
    s.out_fit = ownerdraw(tr(Str::ZoomFit), kIdOutputFit, ButtonKind::Secondary);
    s.out_keep = ownerdraw(tr(Str::KeepAspect), kIdKeepAspect, ButtonKind::Checkbox);
    // Same self-painting slider as the zoom control; it differs only in the
    // range it maps to, which the window answers for.
    s.out_size_slider = make(L"STATIC", L"", SS_NOTIFY, 0, kIdSizeSlider);
    SetWindowLongPtrW(s.out_size_slider, GWLP_USERDATA, kMinOutputEdgePx);
    SetWindowSubclass(s.out_size_slider, &slider_subclass, 1,
                      reinterpret_cast<DWORD_PTR>(this));

    // --- hotkeys ---
    s.hk_caption = make(L"STATIC", tr(Str::SectionHotkeysView), base, 0, kIdLabel);
    s.hk_caption2 = make(L"STATIC", tr(Str::SectionHotkeysWindow), base, 0, kIdLabel);
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        const auto action = static_cast<HotkeyAction>(i);
        const int id = kIdHotkeyFirst + static_cast<int>(i);
        s.hk_label[i] = make(L"STATIC", tr(hotkey_label(action)), base, 0, kIdLabel);
        s.hk_edit[i] = make(L"EDIT", L"", edit | ES_READONLY | ES_CENTER, edit_ex, id);
    }

    // --- interaction ---
    s.act_caption = make(L"STATIC", tr(Str::SectionInteraction), base, 0, kIdLabel);
    s.pass_through = ownerdraw(tr(Str::PassThroughNow), kIdPassThrough, ButtonKind::Secondary);
    s.strict = ownerdraw(tr(Str::StrictCompat), kIdStrictCompat, ButtonKind::Checkbox);
    s.dwell_check = ownerdraw(tr(Str::EdgeDwell), kIdEdgeDwellEnable, ButtonKind::Checkbox);
    s.dwell_ms = make(L"EDIT", L"500", edit | ES_NUMBER, edit_ex, kIdEdgeDwellMs);
    s.dwell_unit = make(L"STATIC", tr(Str::EdgeDwellUnit), base, 0, kIdLabel);

    // --- rendering ---
    s.rnd_caption = make(L"STATIC", tr(Str::SectionRendering), base, 0, kIdLabel);
    s.filter_caption = make(L"STATIC", tr(Str::Filter), base, 0, kIdLabel);
    for (int i = 0; i < 4; ++i) {
        s.filter[i] = ownerdraw(L"", kIdFilterFirst + i, ButtonKind::Segment);
    }
    s.exclude = ownerdraw(tr(Str::ExcludeCapture), kIdExcludeCapture, ButtonKind::Checkbox);
    s.border = ownerdraw(tr(Str::ShowBorder), kIdShowBorder, ButtonKind::Checkbox);

    // --- status and actions ---
    s.status_text =
        make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, kIdStatusLine);
    s.pick = ownerdraw(tr(Str::PickRegion), kIdPickRegion, ButtonKind::Secondary);
    s.startstop = ownerdraw(tr(Str::StartMagnifier), kIdStartStop, ButtonKind::Primary);
    s.save = ownerdraw(tr(Str::SaveSettings), kIdSaveSettings, ButtonKind::Secondary);
    s.quit = ownerdraw(tr(Str::Quit), kIdQuit, ButtonKind::Secondary);
    s.restore = ownerdraw(tr(Str::RestoreDefaults), kIdRestoreDefaults, ButtonKind::Secondary);

    HWND buttons[] = {s.lang[0], s.lang[1],  s.out_apply, s.out_fit,      s.pass_through,
                      s.pick,    s.startstop, s.save,     s.quit,         s.restore,
                      s.exclude, s.border,    s.strict,   s.dwell_check,  s.out_keep};
    for (HWND h : buttons) {
        if (h) SetWindowSubclass(h, &button_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    for (int i = 0; i < kPresetCount; ++i) {
        if (s.preset[i]) {
            SetWindowSubclass(s.preset[i], &button_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (s.shape[i]) {
            SetWindowSubclass(s.shape[i], &button_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
        }
        if (s.filter[i]) {
            SetWindowSubclass(s.filter[i], &button_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        if (s.hk_edit[i]) {
            SetWindowSubclass(s.hk_edit[i], &chord_edit_subclass, 1,
                              reinterpret_cast<DWORD_PTR>(this));
        }
    }

    refresh_preset_labels();
    refresh_filter_labels();
}

void ControlWindow::apply_font() {
    auto& s = *impl_;
    const UINT dpi = dpi_for(hwnd_);

    if (s.body_font) DeleteObject(s.body_font);
    if (s.title_font) DeleteObject(s.title_font);
    s.body_font = make_font(dpi, 10, FW_NORMAL);
    s.title_font = make_font(dpi, 15, FW_SEMIBOLD);

    FontPass pass{s.body_font};
    EnumChildWindows(hwnd_, &apply_font_to_child, reinterpret_cast<LPARAM>(&pass));

    // The window title and the zoom read-out carry the larger face; the pass
    // above gave everything else the body face.
    if (s.title) SendMessageW(s.title, WM_SETFONT, reinterpret_cast<WPARAM>(s.title_font), TRUE);
    if (s.zoom_value) {
        SendMessageW(s.zoom_value, WM_SETFONT, reinterpret_cast<WPARAM>(s.title_font), TRUE);
    }
}

// ===========================================================================
// Layout
// ===========================================================================

void ControlWindow::layout_controls(int client_w, int client_h, bool resize_window) {
    auto& s = *impl_;
    const UINT dpi = dpi_for(hwnd_);
    const int pad = scaled(22, dpi);
    const int card_pad = scaled(14, dpi);
    const int row = scaled(29, dpi);
    const int gap = scaled(10, dpi);
    const int title_h = scaled(20, dpi);
    const int chord_w = scaled(140, dpi);
    const int seg_h = scaled(34, dpi);
    const int preset_h = scaled(32, dpi);
    const int slider_h = scaled(28, dpi);
    const int field_h = scaled(28, dpi);
    const int col_gap = scaled(14, dpi);
    const int header_h = scaled(72, dpi);
    const int status_h = scaled(26, dpi);
    const int button_h = scaled(42, dpi);

    // The label column is measured from the widest label rather than guessed.
    // English and Chinese differ by a factor of three in width, and a guessed
    // column is exactly what clipped "显示 / 隐藏放大镜" under its chord field.
    int label_w = scaled(130, dpi);
    {
        HDC dc = GetDC(hwnd_);
        if (dc != nullptr && s.body_font != nullptr) {
            const HGDIOBJ old_font = SelectObject(dc, s.body_font);
            for (std::size_t i = 0; i < kHotkeyCount; ++i) {
                if (!s.hk_label[i]) continue;
                wchar_t text[160] = {};
                GetWindowTextW(s.hk_label[i], text, 160);
                SIZE size{};
                if (GetTextExtentPoint32W(dc, text, static_cast<int>(std::wcslen(text)),
                                          &size)) {
                    label_w = std::max(label_w, static_cast<int>(size.cx) + scaled(16, dpi));
                }
            }
            SelectObject(dc, old_font);
        }
        if (dc != nullptr) ReleaseDC(hwnd_, dc);
    }

    // --- the shape of one card ---------------------------------------------
    //
    // No card's height depends on the column it lands in, because every card
    // reflows inside whatever width it is given. That is what lets the whole
    // arrangement be solved from a list of heights before anything is placed.
    const int hk_row_w = label_w + gap + chord_w;
    // Below this a card's own contents start to collide, so it is the floor for
    // a column. Every card but the chords reflows inside far less than this --
    // the output fields are the widest of them at about 290 -- so it is a chord
    // row that sets the width, and the whole window is only as wide as three of
    // those plus the gaps.
    const int col_min = std::max(scaled(285, dpi), hk_row_w) + 2 * card_pad;

    // Nine and seven: the display and zoom actions first, which keeps the four
    // presets together on one card rather than splitting them across the split.
    const int hk_split = 9;
    const int hk1_rows = hk_split;
    const int hk2_rows = static_cast<int>(kHotkeyCount) - hk_split;

    const int h_zoom = field_h + slider_h + gap + preset_h + gap + field_h + 2 * card_pad;
    const int h_shape = title_h + seg_h + gap + row + 2 * card_pad;
    const int h_output = title_h + 3 * row + slider_h + 3 * gap + 2 * card_pad;
    const int h_act = title_h + 3 * row + 2 * gap + 2 * card_pad;
    const int h_rnd = title_h + 4 * row + 3 * gap + 2 * card_pad;
    const int h_hk1 = title_h + hk1_rows * row + 2 * card_pad;
    const int h_hk2 = title_h + hk2_rows * row + 2 * card_pad;

    // Tallest first. The packer fills whichever column is shortest so far, and
    // letting the two chord cards claim columns before the small cards go
    // looking for gaps is what keeps the columns close to one another in height.
    enum CardIndex { kHk1, kHk2, kZoom, kOutput, kAct, kRnd, kShape, kCardCount };
    const int heights[kCardCount] = {h_hk1, h_hk2, h_zoom, h_output, h_act, h_rnd, h_shape};

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_w = static_cast<int>(work.right - work.left);
    const int work_h = static_cast<int>(work.bottom - work.top);

    // Equal columns, three of them. Equal is what makes the window read as one
    // grid rather than three strips; three keeps the cards short without making
    // any single column so wide that the middle of the window is empty.
    constexpr int kColumns = 3;
    const int natural = kColumns * col_min + (kColumns - 1) * col_gap + 2 * pad;
    const int width = client_w > 0 ? std::max(client_w, natural) : natural;
    const int col_w = (width - 2 * pad - (kColumns - 1) * col_gap) / kColumns;
    s.content_width = width;

    // --- which card goes in which column -----------------------------------
    //
    // Fixed rather than packed. A settings window that rearranges itself when
    // the language changes is worse than one that is occasionally a little
    // taller than it needs to be, and this split keeps the magnification where
    // the eye starts and the columns close to one another in height.
    const int card_col[kCardCount] = {
        2,  // kHk1     chords for display and zoom (the taller of the two)
        1,  // kHk2     chords for the window and the program
        0,  // kZoom
        1,  // kOutput
        2,  // kAct
        0,  // kRnd
        0,  // kShape
    };
    int col_used[kColumns] = {0, 0, 0};
    int col_cards[kColumns] = {0, 0, 0};
    for (int i = 0; i < kCardCount; ++i) {
        const int c = card_col[i];
        col_used[c] += (col_cards[c] > 0 ? gap : 0) + heights[i];
        ++col_cards[c];
    }
    int body_h = 0;
    int most_gaps = 0;
    for (int c = 0; c < kColumns; ++c) {
        body_h = std::max(body_h, col_used[c]);
        most_gaps = std::max(most_gaps, col_cards[c] - 1);
    }

    const int footer_h = gap + status_h + scaled(10, dpi) + button_h + pad;
    const int natural_h = header_h + gap + body_h + footer_h;

    // A window taller than the cards need opens them up a little, and the footer
    // drops to the bottom edge rather than riding up with the last card. Capped,
    // or a very stretched window would look like a list of unrelated boxes.
    int body_gap = gap;
    if (client_h > 0 && most_gaps > 0) {
        const int slack = client_h - natural_h;
        if (slack > 0) body_gap += std::min(scaled(18, dpi), slack / most_gaps);
    }

    auto place = [&](HWND h, int x, int top, int w, int hgt) {
        if (h) MoveWindow(h, x, top, std::max(1, w), std::max(1, hgt), TRUE);
    };

    s.card_count = 0;
    s.content_height = 0;

    // --- header, spanning every column ---
    s.header = RECT{0, 0, width, header_h};
    {
        const int toggle_w = scaled(72, dpi);
        const int toggle_h = scaled(32, dpi);
        const int toggle_y = (s.header.bottom - toggle_h) / 2;
        const int restore_w = scaled(108, dpi);
        const int restore_x = width - pad - 2 * toggle_w - 2 * gap - restore_w;
        place(s.title, pad, 0, std::max(scaled(140, dpi), restore_x - gap - pad), header_h);
        place(s.restore, restore_x, toggle_y, restore_w, toggle_h);
        place(s.lang[0], width - pad - 2 * toggle_w - gap, toggle_y, toggle_w, toggle_h);
        place(s.lang[1], width - pad - toggle_w, toggle_y, toggle_w, toggle_h);
    }

    int col_y[kColumns];
    for (int c = 0; c < kColumns; ++c) col_y[c] = s.header.bottom + gap;
    int body_bottom = s.header.bottom + gap;

    auto next_card = [&](int index) {
        const int c = card_col[index];
        const int x = pad + c * (col_w + col_gap);
        const RECT r{x, col_y[c], x + col_w, col_y[c] + heights[index]};
        col_y[c] = r.bottom + body_gap;
        body_bottom = std::max(body_bottom, static_cast<int>(r.bottom));
        s.cards[s.card_count++] = r;
        return r;
    };
    auto card_in = [&](const RECT& r) {
        return RECT{r.left + card_pad, r.top + card_pad, r.right - card_pad, r.bottom - card_pad};
    };

    // --- magnification ---
    {
        const RECT in = card_in(next_card(kZoom));
        const int iw = in.right - in.left;
        int cy = in.top;
        const int value_w = scaled(96, dpi);
        place(s.zoom_caption, in.left, cy, std::max(scaled(60, dpi), iw - value_w - gap), field_h);
        place(s.zoom_value, in.right - value_w, cy, value_w, field_h);
        cy += field_h;
        place(s.slider, in.left, cy, iw, slider_h);
        cy += slider_h + gap;
        const int preset_gap = scaled(8, dpi);
        const int preset_w = (iw - preset_gap * (kPresetCount - 1)) / kPresetCount;
        for (int i = 0; i < kPresetCount; ++i) {
            place(s.preset[i], in.left + i * (preset_w + preset_gap), cy, preset_w, preset_h);
        }
        // The editable factor under each button, centred on it: the button jumps
        // to the preset, the field says what the preset is.
        cy += preset_h + gap;
        const int preset_edit_w = scaled(76, dpi);
        for (int i = 0; i < kPresetCount; ++i) {
            const int x = in.left + i * (preset_w + preset_gap) + (preset_w - preset_edit_w) / 2;
            place(s.preset_edit[i], x, cy, preset_edit_w, field_h);
        }
    }

    // --- selection shape ---
    {
        const RECT in = card_in(next_card(kShape));
        const int iw = in.right - in.left;
        place(s.shape_caption, in.left, in.top, iw, title_h);
        const int seg_gap = scaled(8, dpi);
        const int seg_w = (iw - seg_gap * 3) / 4;
        const int seg_y = in.top + title_h;
        for (int i = 0; i < 4; ++i) {
            place(s.shape[i], in.left + i * (seg_w + seg_gap), seg_y, seg_w, seg_h);
        }

        // The kept regions sit under the shapes: four numbered buttons to switch
        // between them, and one button that stores the region now in force into
        // whichever of the four is lit.
        const int slot_y = seg_y + seg_h + gap;
        const int save_w = scaled(104, dpi);
        const int slot_gap = scaled(8, dpi);
        const int slot_w = (iw - save_w - gap - slot_gap * (kSelectionSlotCount - 1)) /
                           kSelectionSlotCount;
        for (int i = 0; i < kSelectionSlotCount; ++i) {
            place(s.slot[i], in.left + i * (slot_w + slot_gap), slot_y, slot_w, row);
        }
        place(s.save_selection, in.right - save_w, slot_y, save_w, row);
    }

    // --- output window: caption, then the two fields, then the buttons ------
    {
        const RECT in = card_in(next_card(kOutput));
        const int iw = in.right - in.left;
        int cy = in.top;
        place(s.out_caption, in.left, cy, iw, title_h);
        cy += title_h;
        const int lw = scaled(40, dpi);
        const int fw = scaled(72, dpi);
        int cx = in.left;
        place(s.out_w_label, cx, cy, lw, row);
        cx += lw;
        place(s.out_w, cx, cy, fw, row);
        cx += fw + gap;
        place(s.out_h_label, cx, cy, lw, row);
        cx += lw;
        place(s.out_h, cx, cy, fw, row);
        cy += row + gap;
        // The drag bar sits between the numbers it drives and the buttons that
        // commit them, so it reads as a third way to set the same value.
        place(s.out_size_slider, in.left, cy, iw, slider_h);
        cy += slider_h + gap;
        const int apply_w = scaled(84, dpi);
        place(s.out_apply, in.left, cy, apply_w, row);
        place(s.out_fit, in.left + apply_w + gap, cy,
              std::max(scaled(60, dpi), iw - apply_w - gap), row);
        cy += row + gap;
        place(s.out_keep, in.left, cy, iw, row);
    }

    // --- interaction ---
    {
        const RECT in = card_in(next_card(kAct));
        const int iw = in.right - in.left;
        int cy = in.top;
        place(s.act_caption, in.left, cy, iw, title_h);
        cy += title_h;
        const int pt_w = scaled(150, dpi);
        place(s.pass_through, in.left, cy, pt_w, row);
        place(s.strict, in.left + pt_w + gap, cy, iw - pt_w - gap, row);
        cy += row + gap;
        place(s.dwell_check, in.left, cy, iw, row);
        cy += row + gap;
        const int dwell_w = scaled(76, dpi);
        place(s.dwell_ms, in.left, cy, dwell_w, row);
        place(s.dwell_unit, in.left + dwell_w + gap, cy,
              std::max(scaled(60, dpi), iw - dwell_w - gap), row);
    }

    // --- rendering ---
    {
        const RECT in = card_in(next_card(kRnd));
        const int iw = in.right - in.left;
        int cy = in.top;
        place(s.rnd_caption, in.left, cy, iw, title_h);
        cy += title_h;
        place(s.filter_caption, in.left, cy, iw, row);
        cy += row + gap;
        const int fgap = scaled(6, dpi);
        const int fw = (iw - 3 * fgap) / 4;
        for (int i = 0; i < 4; ++i) {
            place(s.filter[i], in.left + i * (fw + fgap), cy, fw, row);
        }
        cy += row + gap;
        place(s.exclude, in.left, cy, iw, row);
        cy += row + gap;
        place(s.border, in.left, cy, iw, row);
    }

    // --- the chords, split across two cards --------------------------------
    {
        const RECT in = card_in(next_card(kHk1));
        place(s.hk_caption, in.left, in.top, in.right - in.left, title_h);
        const int first = in.top + title_h;
        for (int i = 0; i < hk1_rows; ++i) {
            const int y = first + i * row;
            place(s.hk_label[i], in.left, y, label_w, row);
            place(s.hk_edit[i], in.left + label_w + gap, y, chord_w, row);
        }
    }
    {
        const RECT in = card_in(next_card(kHk2));
        place(s.hk_caption2, in.left, in.top, in.right - in.left, title_h);
        const int first = in.top + title_h;
        for (int i = hk_split; i < static_cast<int>(kHotkeyCount); ++i) {
            const int y = first + (i - hk_split) * row;
            place(s.hk_label[i], in.left, y, label_w, row);
            place(s.hk_edit[i], in.left + label_w + gap, y, chord_w, row);
        }
    }

    s.content_height = body_bottom;

    // --- footer, spanning every column ---
    //
    // Pinned to the bottom edge when the window is taller than the cards need,
    // so a stretched window keeps its status line and buttons where they are
    // expected instead of leaving them stranded in the middle.
    {
        int top = s.content_height;
        if (client_h > 0) top = std::max(top, client_h - footer_h);
        int cy = top + gap;
        place(s.status_text, pad, cy, width - 2 * pad, status_h);
        cy += status_h + scaled(10, dpi);
        const int button_w = scaled(122, dpi);
        const int primary_w = scaled(152, dpi);
        place(s.pick, pad, cy, button_w, button_h);
        place(s.startstop, pad + button_w + gap, cy, primary_w, button_h);
        place(s.quit, width - pad - button_w, cy, button_w, button_h);
        place(s.save, width - pad - 2 * button_w - gap, cy, button_w, button_h);
    }

    // The window is resizable, so record the smallest frame this arrangement
    // still fits into; WM_GETMINMAXINFO enforces it, because below this the
    // cards in a column would start to overlap.
    RECT minimum{0, 0, natural, natural_h};
    AdjustWindowRectEx(&minimum, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)));
    s.min_track_w = minimum.right - minimum.left;
    s.min_track_h = minimum.bottom - minimum.top;

    if (resize_window && !sized_once_) {
        sized_once_ = true;
        const int max_h = std::max(scaled(400, dpi), work_h - scaled(40, dpi));

        RECT frame{0, 0, std::min(width, work_w - scaled(20, dpi)),
                   std::min(s.content_height, max_h)};
        AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)));
        const int w = frame.right - frame.left;
        const int h = frame.bottom - frame.top;
        const int x = work.left + (work_w - w) / 2;
        const int y = work.top + (work_h - h) / 2;
        SetWindowPos(hwnd_, nullptr, std::max(static_cast<int>(work.left), x),
                     std::max(static_cast<int>(work.top), y), w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ===========================================================================
// Painting
// ===========================================================================

LRESULT CALLBACK ControlWindow::wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto& s = *impl_;

    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int w = client.right;
            const int h = client.bottom;
            if (w <= 0 || h <= 0) {
                EndPaint(hwnd, &ps);
                return 0;
            }

            const Theme& th = theme();
            // Everything is composed into one bitmap and blitted once, so a
            // repaint can never show a half-drawn frame.
            Bitmap buffer(w, h, PixelFormat32bppPARGB);
            {
                Graphics g(&buffer);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

                LinearGradientBrush page(
                    Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)),
                    th.page_top, th.page_bottom, 90.0f);
                g.FillRectangle(&page, 0, 0, w, h);

                SolidBrush header_brush(Gdiplus::Color(255, 0xFF, 0xFF, 0xFF));
                g.FillRectangle(&header_brush, 0, 0, w, s.header.bottom);
                SolidBrush rule(th.separator);
                g.FillRectangle(&rule, 0, s.header.bottom - 1, w, 1);

                for (int i = 0; i < s.card_count; ++i) {
                    const RECT& c = s.cards[i];
                    const Gdiplus::RectF card(
                        static_cast<float>(c.left), static_cast<float>(c.top),
                        static_cast<float>(c.right - c.left),
                        static_cast<float>(c.bottom - c.top));
                    ui::drop_shadow(g, card, 14.0f, 7.0f, 58);
                    fill_round(g, card, 14.0f, th.card);
                    stroke_round(g,
                                 Gdiplus::RectF(card.X + 0.5f, card.Y + 0.5f, card.Width - 1.0f,
                                                card.Height - 1.0f),
                                 14.0f, th.card_edge, 1.0f);
                }
            }

            Graphics screen(dc);
            screen.DrawImage(&buffer, 0, 0, w, h);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (dis->CtlType != ODT_BUTTON) break;
            const int id = static_cast<int>(dis->CtlID);
            const HWND control = dis->hwndItem;
            const ButtonKind kind = button_role(control);
            bool checked = button_checked(control);

            // A segmented control states its selection in the window's state,
            // not in its own property: several of them can be selected at once
            // only if the owner decides which.
            if (kind == ButtonKind::Segment) {
                if (id >= kIdShapeFirst && id < kIdShapeFirst + 4) {
                    checked = cfg_.selection_shape == kShapeOrder[id - kIdShapeFirst];
                } else if (id >= kIdPresetFirst && id < kIdPresetFirst + kPresetCount) {
                    checked = cfg_.factor_q16 == cfg_.presets[id - kIdPresetFirst];
                } else if (id >= kIdFilterFirst && id < kIdFilterFirst + 4) {
                    checked = cfg_.scale_filter == kFilterOrder[id - kIdFilterFirst];
                } else if (id >= kIdLangFirst && id < kIdLangFirst + 2) {
                    checked = (id == kIdLangFirst && cfg_.language == Language::Chinese) ||
                              (id == kIdLangFirst + 1 && cfg_.language == Language::English);
                }
            }
            paint_button(dis, kind, checked, s.hover == control,
                         backdrop_under(hwnd, s.cards, s.card_count, s.header, dis->rcItem));
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);

            const bool muted = control == s.zoom_caption || control == s.shape_caption ||
                               control == s.out_caption || control == s.hk_caption ||
                               control == s.hk_caption2 ||
                               control == s.act_caption || control == s.rnd_caption ||
                               control == s.filter_caption || control == s.dwell_unit;
            COLORREF colour = muted ? RGB(0x68, 0x71, 0x82) : RGB(0x1B, 0x20, 0x2B);
            SetTextColor(dc, colour);

            static HBRUSH card = nullptr;
            if (!card) card = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
            return reinterpret_cast<LRESULT>(card);
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
            // The factor box carries the accent the read-out it replaced had.
            SetTextColor(dc, reinterpret_cast<HWND>(lparam) == s.zoom_value
                                 ? RGB(0x2A, 0x5C, 0xC4)
                                 : RGB(0x1B, 0x20, 0x2B));
            static HBRUSH field = nullptr;
            if (!field) field = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
            return reinterpret_cast<LRESULT>(field);
        }

        case kMsgSliderMoved: {
            const Q16 factor = q16_from_slider_pos(static_cast<int>(wparam));
            set_text(s.zoom_value, format_factor_text(factor).c_str());
            if (callbacks_.on_factor_changed) callbacks_.on_factor_changed(factor);
            return 0;
        }

        case kMsgSliderRange: {
            const HWND which = reinterpret_cast<HWND>(lparam);
            int lo = kSliderMin;
            int hi = kSliderMax;
            if (GetDlgCtrlID(which) == kIdSizeSlider) {
                // Anywhere from the smallest window the controller will accept
                // up to the full width of the desktop.
                lo = kMinOutputEdgePx;
                hi = std::max<int>(lo + 1, s.desktop.width);
            }
            return MAKELONG(lo, hi);
        }

        case kMsgSizeMoved:
            apply_size_slider(static_cast<Px>(static_cast<int>(wparam)));
            return 0;

        case WM_COMMAND:
            on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;

        case WM_TIMER:
            if (wparam == kFactorCommitTimer) {
                KillTimer(hwnd, kFactorCommitTimer);
                commit_factor_edit(false);
                if (s.preset_editing >= 0) commit_preset_edit(s.preset_editing, false);
                return 0;
            }
            if (wparam == kSaveConfirmTimer) {
                KillTimer(hwnd, kSaveConfirmTimer);
                set_text(s.save_selection, tr(Str::SaveSelection));
                return 0;
            }
            break;

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                layout_controls(LOWORD(lparam), HIWORD(lparam), false);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;

        case WM_DPICHANGED: {
            apply_font();
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout_controls(0, 0, false);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
            // Whatever the current layout actually needs, so a resize cannot
            // shrink a control out of reach. Computed by layout_controls().
            if (s.min_track_w > 0 && s.min_track_h > 0) {
                mmi->ptMinTrackSize.x = s.min_track_w;
                mmi->ptMinTrackSize.y = s.min_track_h;
            }
            return 0;
        }

        case WM_CLOSE:
            hide();
            if (on_closed_) on_closed_();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// ===========================================================================
// Commands
// ===========================================================================

int ControlWindow::read_dwell_ms() const {
    wchar_t buf[32] = {};
    GetWindowTextW(impl_->dwell_ms, buf, 32);
    return std::clamp(static_cast<int>(_wtoi(buf)), 100, 5000);
}

void ControlWindow::commit_size_edits() {
    auto& s = *impl_;
    wchar_t buf[32] = {};
    GetWindowTextW(s.out_w, buf, 32);
    const Px w = std::max<Px>(kMinOutputEdgePx, static_cast<Px>(_wtoi(buf)));
    GetWindowTextW(s.out_h, buf, 32);
    const Px h = std::max<Px>(kMinOutputEdgePx, static_cast<Px>(_wtoi(buf)));
    set_text_int(s.out_w, w);
    set_text_int(s.out_h, h);
    if (callbacks_.on_output_size_changed) callbacks_.on_output_size_changed(SizePx{w, h});
}

void ControlWindow::apply_size_slider(Px width) {
    auto& s = *impl_;
    const Px cur_w = std::max<Px>(1, s.last_output.width);
    const Px cur_h = std::max<Px>(1, s.last_output.height);
    const Px max_w = std::max<Px>(kMinOutputEdgePx, s.desktop.width);
    const Px max_h = std::max<Px>(kMinOutputEdgePx, s.desktop.height);

    Px w = std::clamp<Px>(width, kMinOutputEdgePx, max_w);
    Px h = std::clamp<Px>(cur_h, kMinOutputEdgePx, max_h);
    if (cfg_.keep_aspect_ratio) {
        // The slider scales the window's own shape rather than its width alone,
        // so dragging it changes how much screen the window covers without
        // changing how much of the selection it is showing.
        h = std::clamp<Px>(static_cast<Px>(static_cast<long long>(w) * cur_h / cur_w),
                           kMinOutputEdgePx, max_h);
        w = std::clamp<Px>(static_cast<Px>(static_cast<long long>(h) * cur_w / cur_h),
                           kMinOutputEdgePx, max_w);
    }
    if (w == s.last_output.width && h == s.last_output.height) return;
    if (callbacks_.on_output_size_changed) callbacks_.on_output_size_changed(SizePx{w, h});
}

void ControlWindow::on_command(int control_id, int notify_code) {
    auto& s = *impl_;

    // An owner-drawn checkbox does not toggle itself, so the state lives here
    // and is flipped on the click before the callback runs.
    auto toggle_check = [&](int id, HWND control, const std::function<void(bool)>& fn) {
        if (!fn) return;
        const bool now = !s.check_of(id);
        fn(now);
        s.checks[id] = now;
        button_set_checked(control, now);
    };

    if (control_id >= kIdLangFirst && control_id < kIdLangFirst + 2) {
        const Language lang = control_id == kIdLangFirst ? Language::Chinese : Language::English;
        if (cfg_.language != lang) {
            cfg_.language = lang;
            set_language(lang);
            retranslate();
            if (callbacks_.on_language_changed) callbacks_.on_language_changed(lang);
        }
        return;
    }

    if (control_id >= kIdSelectionSlotFirst &&
        control_id < kIdSelectionSlotFirst + kSelectionSlotCount) {
        if (callbacks_.on_selection_slot) {
            callbacks_.on_selection_slot(control_id - kIdSelectionSlotFirst);
        }
        return;
    }

    if (control_id >= kIdPresetEditFirst && control_id < kIdPresetEditFirst + kPresetCount) {
        const int index = control_id - kIdPresetEditFirst;
        if (notify_code == EN_SETFOCUS) {
            s.preset_editing = index;
            return;
        }
        if (notify_code == EN_KILLFOCUS) {
            commit_preset_edit(index, true);
            if (s.preset_editing == index) s.preset_editing = -1;
            return;
        }
        if (notify_code == EN_CHANGE) {
            // Same debounce as the factor box, for the same reason: a number
            // should take effect once typing pauses, and the window's own
            // writes raise this too.
            if (s.writing_factor) return;
            s.preset_editing = index;
            KillTimer(hwnd_, kFactorCommitTimer);
            SetTimer(hwnd_, kFactorCommitTimer, kFactorCommitDelayMs, nullptr);
            return;
        }
        return;
    }

    if (control_id >= kIdShapeFirst && control_id < kIdShapeFirst + 4) {
        if (callbacks_.on_shape_changed) {
            callbacks_.on_shape_changed(kShapeOrder[control_id - kIdShapeFirst]);
        }
        return;
    }

    if (control_id >= kIdPresetFirst && control_id < kIdPresetFirst + kPresetCount) {
        if (callbacks_.on_factor_changed) {
            callbacks_.on_factor_changed(cfg_.presets[control_id - kIdPresetFirst]);
        }
        return;
    }

    if (control_id >= kIdFilterFirst && control_id < kIdFilterFirst + 4) {
        if (callbacks_.on_filter_changed) {
            callbacks_.on_filter_changed(kFilterOrder[control_id - kIdFilterFirst]);
        }
        return;
    }

    if (control_id >= kIdHotkeyFirst &&
        control_id < kIdHotkeyFirst + static_cast<int>(kHotkeyCount)) {
        return;  // the edit's subclass owns this
    }

    switch (control_id) {
        case kIdOutputApply:
            commit_size_edits();
            return;
        case kIdOutputFit:
            // An empty size means "the region at the current factor", which is
            // the window shape that shows all of it and nothing else.
            if (callbacks_.on_output_size_changed) callbacks_.on_output_size_changed(SizePx{0, 0});
            return;
        case kIdKeepAspect:
            toggle_check(kIdKeepAspect, s.out_keep, callbacks_.on_keep_aspect_changed);
            return;
        case kIdPassThrough:
            if (callbacks_.on_toggle_pass_through) callbacks_.on_toggle_pass_through();
            return;
        case kIdStrictCompat:
            toggle_check(kIdStrictCompat, s.strict, callbacks_.on_strict_compat_changed);
            return;
        case kIdEdgeDwellEnable:
            toggle_check(kIdEdgeDwellEnable, s.dwell_check, nullptr);
            if (callbacks_.on_edge_dwell_changed) {
                callbacks_.on_edge_dwell_changed(cfg_.edge_dwell_band_px, read_dwell_ms(),
                                                 s.check_of(kIdEdgeDwellEnable));
            }
            return;
        case kIdEdgeDwellMs:
            // Only when the user is finished. SetWindowText on this field also
            // raises EN_UPDATE and EN_CHANGE, so reacting to those would call
            // back into itself until the stack ran out.
            if (notify_code != EN_KILLFOCUS) return;
            if (callbacks_.on_edge_dwell_changed) {
                const int ms = read_dwell_ms();
                if (ms != cfg_.edge_dwell_ms) set_text_int(s.dwell_ms, ms);
                callbacks_.on_edge_dwell_changed(cfg_.edge_dwell_band_px, ms,
                                                 s.check_of(kIdEdgeDwellEnable));
            }
            return;
        case kIdFactorLabel:
            // The factor box is the one field that applies while it is being
            // used rather than only when it is left: typing a number and having
            // it take effect is the whole point of having the box at all.
            if (notify_code == EN_SETFOCUS) {
                s.factor_editing = true;
                return;
            }
            if (notify_code == EN_KILLFOCUS) {
                s.factor_editing = false;
                KillTimer(hwnd_, kFactorCommitTimer);
                commit_factor_edit(true);
                return;
            }
            if (notify_code == EN_CHANGE) {
                // Writing the box raises this too, and the window syncs several
                // times a second, so without this the timer below would be
                // killed and re-armed by our own writes and never once fire.
                if (s.writing_factor) return;
                // Restarted on every keystroke, so typing "10" applies 10 and
                // never pauses long enough to apply 1 on the way.
                KillTimer(hwnd_, kFactorCommitTimer);
                SetTimer(hwnd_, kFactorCommitTimer, kFactorCommitDelayMs, nullptr);
                return;
            }
            return;
        case kIdSaveSelection:
            if (callbacks_.on_save_selection) callbacks_.on_save_selection();
            // Confirmed on the button itself, not only in the status line: the
            // write target is the lit number and it does not move when the
            // region does, so a second save with everything already in place
            // used to change nothing on screen at all.
            set_text(s.save_selection, tr(Str::SelectionSavedMark));
            SetTimer(hwnd_, kSaveConfirmTimer, kSaveConfirmMs, nullptr);
            return;
        case kIdExcludeCapture:
            toggle_check(kIdExcludeCapture, s.exclude, callbacks_.on_exclude_from_capture_changed);
            return;
        case kIdShowBorder:
            toggle_check(kIdShowBorder, s.border, callbacks_.on_show_border_changed);
            return;
        case kIdPickRegion:
            if (callbacks_.on_pick_region) callbacks_.on_pick_region();
            return;
        case kIdStartStop:
            if (callbacks_.on_toggle_magnifier) callbacks_.on_toggle_magnifier();
            return;
        case kIdSaveSettings:
            commit_size_edits();
            if (callbacks_.on_save_config) callbacks_.on_save_config();
            return;
        case kIdRestoreDefaults:
            if (callbacks_.on_restore_defaults) callbacks_.on_restore_defaults();
            return;
        case kIdQuit:
            if (callbacks_.on_quit) callbacks_.on_quit();
            return;
        default:
            break;
    }
}

// ===========================================================================
// Chord capture
// ===========================================================================

void ControlWindow::begin_chord_capture(HWND edit) {
    auto& s = *impl_;
    int index = -1;
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        if (s.hk_edit[i] == edit) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) return;

    // The focus move comes first, and the arming after it. SetFocus() delivers
    // WM_KILLFOCUS *synchronously* to whatever had the keyboard -- and if that
    // was another chord field, its handler cancels the capture, which resets
    // chord_capture_index_ to -1. Arming before the move therefore left the new
    // field showing "press a key" while nothing was armed at all, so every key
    // after it was ignored: changing one hotkey and then clicking the next
    // field made the second one dead.
    SetFocus(edit);
    chord_capture_index_ = index;
    SetWindowTextW(edit, tr(Str::PressAKey));
    // Every chord the program owns has to stop being registered while the user
    // is typing one: RegisterHotKey consumes its own chords, so pressing the one
    // that is already taken -- which is exactly what someone reassigning a
    // hotkey does -- fired the action and the field never saw the key at all.
    if (callbacks_.on_chord_capture) callbacks_.on_chord_capture(true);
}

bool ControlWindow::is_armed_field(HWND edit) const noexcept {
    if (chord_capture_index_ < 0) return false;
    const auto& s = *impl_;
    return s.hk_edit[static_cast<std::size_t>(chord_capture_index_)] == edit;
}

void ControlWindow::cancel_chord_capture() {
    if (chord_capture_index_ < 0) return;
    chord_capture_index_ = -1;
    if (callbacks_.on_chord_capture) callbacks_.on_chord_capture(false);
    refresh_hotkey_labels();
}

void ControlWindow::handle_chord_key(UINT vk) {
    if (chord_capture_index_ < 0) return;
    if (vk == VK_ESCAPE || vk == VK_TAB) {
        cancel_chord_capture();
        return;
    }
    if (is_modifier_key(vk)) return;   // wait for the key the chord ends on

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool win = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;

    // A bare key would swallow that key everywhere; function keys are the one
    // case users expect to be allowed on their own.
    const bool is_function_key = vk >= VK_F1 && vk <= VK_F24;
    if (!ctrl && !alt && !shift && !win && !is_function_key) {
        // Refused, but not silently: a field that ignores the press looks broken
        // in exactly the way this did. The capture stays armed, so the next key
        // -- this time with a modifier -- still lands.
        auto& s = *impl_;
        if (s.hk_edit[static_cast<std::size_t>(chord_capture_index_)]) {
            SetWindowTextW(s.hk_edit[static_cast<std::size_t>(chord_capture_index_)],
                           tr(Str::HotkeyNeedsModifier));
        }
        return;
    }

    commit_captured_chord(vk);
}

void ControlWindow::handle_chord_mouse_button(UINT message, WPARAM wparam) {
    if (chord_capture_index_ < 0) return;

    // A mouse button is allowed on its own, unlike a letter: nothing else uses
    // a side button, and "Ctrl + the back button" is not the gesture anyone
    // reaches for. It does mean the button belongs to the program while it is
    // bound -- see InputThread::ll_mouse_proc.
    switch (message) {
        case WM_RBUTTONDOWN:
            commit_captured_chord(VK_RBUTTON);
            return;
        case WM_MBUTTONDOWN:
            commit_captured_chord(VK_MBUTTON);
            return;
        case WM_XBUTTONDOWN:
            if (HIWORD(wparam) == XBUTTON1) {
                commit_captured_chord(VK_XBUTTON1);
            } else if (HIWORD(wparam) == XBUTTON2) {
                commit_captured_chord(VK_XBUTTON2);
            }
            return;
        default:
            return;
    }
}

void ControlWindow::commit_captured_chord(UINT vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool win = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;

    HotkeyChord chord{};
    if (ctrl) chord.modifiers |= 0x0002;   // MOD_CONTROL
    if (alt) chord.modifiers |= 0x0001;    // MOD_ALT
    if (shift) chord.modifiers |= 0x0004;  // MOD_SHIFT
    if (win) chord.modifiers |= 0x0008;    // MOD_WIN
    chord.virtual_key = vk;

    std::array<HotkeyChord, kHotkeyCount> next = cfg_.hotkeys;
    const std::size_t index = static_cast<std::size_t>(chord_capture_index_);
    next[index] = chord;
    chord_capture_index_ = -1;

    // Registration comes back before the new table goes in, so the chords are
    // never left released if the rebind callback does nothing.
    if (callbacks_.on_chord_capture) callbacks_.on_chord_capture(false);
    if (callbacks_.on_hotkeys_changed) {
        callbacks_.on_hotkeys_changed(next);
    } else {
        cfg_.hotkeys = next;
        refresh_hotkey_labels();
    }
}

void ControlWindow::refresh_hotkey_labels() {
    auto& s = *impl_;
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        if (!s.hk_edit[i]) continue;
        std::wstring text;
        if (chord_capture_index_ == static_cast<int>(i)) {
            text = tr(Str::PressAKey);
        } else if (chord_is_bound(cfg_.hotkeys[i])) {
            text = utf8_to_wide(describe_chord(cfg_.hotkeys[i]));
        } else {
            text = tr(Str::Unbound);
        }
        SetWindowTextW(s.hk_edit[i], text.c_str());
    }
}

void ControlWindow::refresh_preset_labels() {
    auto& s = *impl_;
    for (int i = 0; i < kPresetCount; ++i) {
        if (!s.preset[i]) continue;
        const double factor = q16_to_double(cfg_.presets[i]);
        wchar_t buf[32];
        if (factor == static_cast<double>(static_cast<long long>(factor))) {
            std::swprintf(buf, 32, L"%lldx", static_cast<long long>(factor));
        } else {
            std::swprintf(buf, 32, L"%.2gx", factor);
        }
        SetWindowTextW(s.preset[i], buf);
    }
}

void ControlWindow::refresh_filter_labels() {
    auto& s = *impl_;
    static const Str kNames[4] = {Str::FilterAuto, Str::FilterPoint, Str::FilterBilinear,
                                  Str::FilterBicubic};
    for (int i = 0; i < 4; ++i) set_text(s.filter[i], tr(kNames[i]));
}

void ControlWindow::write_factor_text() {
    auto& s = *impl_;
    if (!s.zoom_value) return;
    // Flagged so the EN_CHANGE this raises is not taken for the user typing.
    s.writing_factor = true;
    set_text(s.zoom_value, format_factor_text(cfg_.factor_q16).c_str());
    s.writing_factor = false;
}

void ControlWindow::write_preset_text(int index) {
    auto& s = *impl_;
    if (index < 0 || index >= kPresetCount || !s.preset_edit[index]) return;
    s.writing_factor = true;
    set_text(s.preset_edit[index], format_factor_text(cfg_.presets[index]).c_str());
    s.writing_factor = false;
}

void ControlWindow::commit_preset_edit(int index, bool rewrite) {
    auto& s = *impl_;
    if (index < 0 || index >= kPresetCount || !s.preset_edit[index]) return;
    wchar_t buf[64] = {};
    GetWindowTextW(s.preset_edit[index], buf, 64);
    Q16 parsed = cfg_.presets[index];
    const bool ok = parse_factor_text(buf, parsed);
    if (!ok) {
        // Nothing usable in the field: put the preset back, but only once the
        // user has finished with it, or the caret would be moved mid-number.
        if (rewrite) write_preset_text(index);
        return;
    }
    const Q16 previous = cfg_.presets[index];
    cfg_.presets[index] = parsed;
    if (rewrite) write_preset_text(index);
    if (cfg_.presets[index] != previous) {
        // Refreshed here rather than left to sync(): cfg_ was already updated
        // above, so the sync that follows sees no change and would leave the
        // button still showing the old value.
        refresh_preset_labels();
        if (callbacks_.on_presets_changed) callbacks_.on_presets_changed(cfg_.presets);
    }
}

void ControlWindow::refresh_factor_label() {
    auto& s = *impl_;
    // Never overwrite the box while it owns the keyboard: the window syncs
    // several times a second and would eat the number as it is typed.
    if (!s.factor_editing) write_factor_text();
    if (s.slider) {
        SendMessageW(s.slider, kMsgSliderSet,
                     static_cast<WPARAM>(slider_pos_from_q16(cfg_.factor_q16)), 0);
    }
}

void ControlWindow::commit_factor_edit(bool rewrite) {
    auto& s = *impl_;
    if (!s.zoom_value) return;

    wchar_t buf[64] = {};
    GetWindowTextW(s.zoom_value, buf, 64);
    Q16 applied = cfg_.factor_q16;
    const bool parsed = parse_factor_text(buf, applied);

    const Q16 previous = cfg_.factor_q16;
    if (parsed) cfg_.factor_q16 = applied;
    if (cfg_.factor_q16 != previous && callbacks_.on_factor_changed) {
        callbacks_.on_factor_changed(cfg_.factor_q16);
    }
    // Written after the callback, which round-trips through the app and pushes a
    // snapshot straight back through sync(); drawing the box first would leave
    // the last word to that round trip. Half-typed input is left alone until the
    // field is done with, so the caret is never moved mid-number.
    if (parsed || rewrite) write_factor_text();
    if (s.slider) {
        SendMessageW(s.slider, kMsgSliderSet,
                     static_cast<WPARAM>(slider_pos_from_q16(cfg_.factor_q16)), 0);
    }
}

void ControlWindow::retranslate() {
    auto& s = *impl_;
    SetWindowTextW(hwnd_, tr(Str::AppTitle));
    set_text(s.title, tr(Str::AppTitle));
    set_text(s.lang[0], tr(Str::LanguageChinese));
    set_text(s.lang[1], tr(Str::LanguageEnglish));
    set_text(s.zoom_caption, tr(Str::SectionZoom));
    set_text(s.shape_caption, tr(Str::SectionShape));
    set_text(s.out_caption, tr(Str::SectionWindow));
    set_text(s.hk_caption, tr(Str::SectionHotkeysView));
    set_text(s.hk_caption2, tr(Str::SectionHotkeysWindow));
    set_text(s.act_caption, tr(Str::SectionInteraction));
    set_text(s.rnd_caption, tr(Str::SectionRendering));
    set_text(s.out_w_label, tr(Str::Width));
    set_text(s.out_h_label, tr(Str::Height));
    set_text(s.out_apply, tr(Str::Apply));
    set_text(s.out_fit, tr(Str::ZoomFit));
    set_text(s.out_keep, tr(Str::KeepAspect));
    set_text(s.pass_through, tr(Str::PassThroughNow));
    set_text(s.strict, tr(Str::StrictCompat));
    set_text(s.dwell_check, tr(Str::EdgeDwell));
    set_text(s.dwell_unit, tr(Str::EdgeDwellUnit));
    set_text(s.filter_caption, tr(Str::Filter));
    set_text(s.exclude, tr(Str::ExcludeCapture));
    set_text(s.border, tr(Str::ShowBorder));
    set_text(s.pick, tr(Str::PickRegion));
    set_text(s.save, tr(Str::SaveSettings));
    set_text(s.quit, tr(Str::Quit));
    set_text(s.restore, tr(Str::RestoreDefaults));
    set_text(s.save_selection, tr(Str::SaveSelection));
    // The start/stop button carries the state as well as the language, and sync()
    // only rewrites it when the state changes -- so a switch of language left it
    // in the one it was labelled in.
    set_text(s.startstop, tr(s.last_state == InteractionState::Off ? Str::StartMagnifier
                                                                   : Str::StopMagnifier));

    for (int i = 0; i < 4; ++i) set_text(s.shape[i], tr(shape_label(kShapeOrder[i])));
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        set_text(s.hk_label[i], tr(hotkey_label(static_cast<HotkeyAction>(i))));
    }
    refresh_hotkey_labels();
    refresh_filter_labels();

    // The labels changed width, so the layout has to be recomputed from them.
    layout_controls(s.content_width, 0, false);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ===========================================================================
// Public state
// ===========================================================================

HWND ControlWindow::hovered() const noexcept {
    return impl_ ? impl_->hover : nullptr;
}

void ControlWindow::set_hovered(HWND control) noexcept {
    if (impl_) impl_->hover = control;
}

void ControlWindow::show() {
    if (hwnd_) ShowWindow(hwnd_, SW_SHOWNORMAL);
}

void ControlWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void ControlWindow::bring_to_front() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void ControlWindow::set_status(const std::string& status) {
    if (impl_->status_text) SetWindowTextW(impl_->status_text, utf8_to_wide(status).c_str());
}

void ControlWindow::set_capture_state(const std::string& backend, bool fallback, bool suspended) {
    auto& s = *impl_;
    std::wstring text =
        backend.empty() ? std::wstring(tr(Str::CaptureDxgi)) : utf8_to_wide(backend);
    if (fallback) {
        text += L"  (";
        text += tr(Str::CaptureFallbackNote);
        text += L")";
    }
    if (suspended) {
        text += L"  [";
        text += tr(Str::StateSuspended);
        text += L"]";
    }
    if (s.status_text) SetWindowTextW(s.status_text, text.c_str());
}

void ControlWindow::sync(const AppConfig& cfg, InteractionState state, const RenderStats& stats,
                         const RectPx& selection_px, const RectPx& output_px,
                         const std::string& status, const std::string& capture_backend,
                         bool capture_fallback) {
    if (!hwnd_) return;
    auto& s = *impl_;

    // This runs on every event and the zoom slider publishes one per tick, so
    // each widget is only touched when its value actually changed.
    const AppConfig previous = cfg_;
    const bool have_previous = s.have_synced;
    cfg_ = cfg;
    s.have_synced = true;

    refresh_factor_label();

    if (!have_previous || previous.presets != cfg_.presets) {
        refresh_preset_labels();
        for (int i = 0; i < kPresetCount; ++i) {
            // Never over the field being typed into.
            if (i != s.preset_editing) write_preset_text(i);
        }
    }
    if (!have_previous || previous.selection_slot != cfg_.selection_slot) {
        for (int i = 0; i < kSelectionSlotCount; ++i) {
            button_set_checked(s.slot[i], i == cfg_.selection_slot);
        }
    }
    if (!have_previous || previous.hotkeys != cfg_.hotkeys) refresh_hotkey_labels();

    s.checks[kIdKeepAspect] = cfg_.keep_aspect_ratio;
    s.checks[kIdStrictCompat] = cfg_.strict_compat_mode;
    s.checks[kIdEdgeDwellEnable] = cfg_.edge_dwell_enabled;
    s.checks[kIdExcludeCapture] = cfg_.exclude_self_from_capture;
    s.checks[kIdShowBorder] = cfg_.show_border;
    button_set_checked(s.out_keep, cfg_.keep_aspect_ratio);
    button_set_checked(s.strict, cfg_.strict_compat_mode);
    button_set_checked(s.dwell_check, cfg_.edge_dwell_enabled);
    button_set_checked(s.exclude, cfg_.exclude_self_from_capture);
    button_set_checked(s.border, cfg_.show_border);

    if (!have_previous || previous.selection_shape != cfg_.selection_shape) {
        for (int i = 0; i < 4; ++i) InvalidateRect(s.shape[i], nullptr, FALSE);
    }
    if (!have_previous || previous.factor_q16 != cfg_.factor_q16 ||
        previous.presets != cfg_.presets) {
        for (int i = 0; i < kPresetCount; ++i) InvalidateRect(s.preset[i], nullptr, FALSE);
    }
    if (!have_previous || previous.scale_filter != cfg_.scale_filter) {
        for (int i = 0; i < 4; ++i) InvalidateRect(s.filter[i], nullptr, FALSE);
    }
    if (!have_previous || previous.language != cfg_.language) {
        InvalidateRect(s.lang[0], nullptr, FALSE);
        InvalidateRect(s.lang[1], nullptr, FALSE);
    }

    const int out_w = width_of(output_px);
    const int out_h = height_of(output_px);
    if (!have_previous || s.last_out_w != out_w) {
        s.last_out_w = out_w;
        set_text_int(s.out_w, out_w);
    }
    if (!have_previous || s.last_out_h != out_h) {
        s.last_out_h = out_h;
        set_text_int(s.out_h, out_h);
    }
    // The size slider reads the same number the boxes do, so the three controls
    // can never disagree about how big the window is.
    s.last_output = SizePx{out_w, out_h};
    s.desktop = SizePx{GetSystemMetrics(SM_CXVIRTUALSCREEN),
                       GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (s.out_size_slider) {
        SendMessageW(s.out_size_slider, kMsgSliderSet, static_cast<WPARAM>(out_w), 0);
    }
    if (!have_previous || previous.edge_dwell_ms != cfg_.edge_dwell_ms) {
        set_text_int(s.dwell_ms, cfg_.edge_dwell_ms);
    }

    if (!have_previous || s.last_state != state) {
        s.last_state = state;
        set_text(s.startstop,
                 tr(state == InteractionState::Off ? Str::StartMagnifier : Str::StopMagnifier));
    }

    // With the magnifier off the line has to say how to turn it on: the program
    // opens with no magnified window and no explanation of what to press, which
    // is the first thing a new user meets and the first place to conclude that
    // it does not work.
    wchar_t line[512];
    std::swprintf(line, 512, L"%ls%ls   %ls %dx%d @ (%d, %d)   %ls %dx%d   %.0f fps   %ls: %ls%ls",
                  tr(state_label(state)),
                  state == InteractionState::Off ? tr(Str::StatusOffHint) : L"",
                  tr(Str::StatusSelection), width_of(selection_px),
                  height_of(selection_px), selection_px.left, selection_px.top,
                  tr(Str::StatusOutput), width_of(output_px), height_of(output_px),
                  stats.present_fps, tr(Str::StatusCapture),
                  capture_backend.empty() ? tr(Str::CaptureDxgi)
                                          : utf8_to_wide(capture_backend).c_str(),
                  capture_fallback ? tr(Str::CaptureGdi) : L"");

    if (!status.empty()) {
        std::wstring full = line;
        full += L"   -- ";
        full += utf8_to_wide(status);
        SetWindowTextW(s.status_text, full.c_str());
        return;
    }
    SetWindowTextW(s.status_text, line);
}

}  // namespace mag
