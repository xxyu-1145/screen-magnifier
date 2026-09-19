// platform/app_icon.cpp — the application icon, drawn with GDI+.
//
// The mark is a magnifier over a rounded tile: recognisable at 16 pixels, where
// the fine detail of most logos turns to mud, and still clean at 256.

#include "platform/app_icon.h"

#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace mag {
namespace {

using namespace Gdiplus;

// A magnifier: the lens ring, the glass inside it, a highlight, and the handle.
// Everything is expressed as a fraction of the tile so one description serves
// every size.
void draw_mark(Graphics& g, float size, float radius) {
    const float lens_cx = size * 0.415f;
    const float lens_cy = size * 0.415f;
    const float lens_r = size * 0.235f;
    const float ring_w = std::max(1.0f, size * 0.092f);
    const float handle_w = ring_w * 0.92f;

    // The glass, faintly tinted so the lens reads as glass and not as a hole.
    {
        SolidBrush glass(Color(64, 0xFF, 0xFF, 0xFF));
        g.FillEllipse(&glass, lens_cx - lens_r, lens_cy - lens_r, lens_r * 2.0f, lens_r * 2.0f);
    }

    // The handle, drawn before the ring so the ring's stroke caps it cleanly.
    {
        const float a = 0.7071f;  // 45 degrees
        const float from_x = lens_cx + a * lens_r * 0.72f;
        const float from_y = lens_cy + a * lens_r * 0.72f;
        const float to_x = from_x + a * size * 0.205f;
        const float to_y = from_y + a * size * 0.205f;
        Pen pen(Color(255, 0xFF, 0xFF, 0xFF), handle_w);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, from_x, from_y, to_x, to_y);
    }

    // The ring.
    {
        Pen pen(Color(255, 0xFF, 0xFF, 0xFF), ring_w);
        g.DrawEllipse(&pen, lens_cx - lens_r, lens_cy - lens_r, lens_r * 2.0f, lens_r * 2.0f);
    }

    // A highlight on the upper-left of the ring, which is what makes the lens
    // look like it is catching light.
    {
        const float hl_r = lens_r * 0.30f;
        const float a = 0.7071f;
        SolidBrush brush(Color(150, 0xFF, 0xFF, 0xFF));
        g.FillEllipse(&brush, lens_cx - a * lens_r * 0.62f - hl_r * 0.5f,
                      lens_cy - a * lens_r * 0.62f - hl_r * 0.5f, hl_r, hl_r);
    }
}

}  // namespace

namespace {

// The id the resource embedder gives the group icon (tools/embed_resources.py).
constexpr int kGroupIconId = 1;

HICON load_embedded_icon(int size_px) noexcept {
    HMODULE self = GetModuleHandleW(nullptr);
    if (self == nullptr) return nullptr;
    // LoadImage picks the closest image in the group, so the tray gets the 16
    // and the title bar the 32 rather than either being resampled.
    return static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(kGroupIconId), IMAGE_ICON,
                                         size_px, size_px, LR_DEFAULTCOLOR));
}

// An `app.ico` beside the executable is a development override; it only ever
// matters for a build that was not embedded.
HICON load_sidecar_icon(int size_px) noexcept {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return nullptr;

    wchar_t* last_slash = std::wcsrchr(path, L'\\');
    if (last_slash == nullptr) return nullptr;
    *(last_slash + 1) = L'\0';

    std::wstring file(path);
    file += L"app.ico";
    return static_cast<HICON>(LoadImageW(nullptr, file.c_str(), IMAGE_ICON, size_px, size_px,
                                         LR_LOADFROMFILE));
}

}  // namespace

HICON make_app_icon(int size_px) noexcept {
    if (size_px < 8 || size_px > 512) return nullptr;

    // In order of authority: the icon compiled into the executable, which is
    // also the one Explorer shows; then a file beside it; then drawn. Preferring
    // the embedded one keeps the window and tray icons identical to the file's,
    // which a stray app.ico would otherwise silently break.
    if (HICON embedded = load_embedded_icon(size_px)) return embedded;
    if (HICON custom = load_sidecar_icon(size_px)) return custom;

    const float size = static_cast<float>(size_px);
    Bitmap bitmap(size_px, size_px, PixelFormat32bppARGB);
    if (bitmap.GetLastStatus() != Ok) return nullptr;

    {
        Graphics g(&bitmap);
        if (g.GetLastStatus() != Ok) return nullptr;
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const float radius = size * 0.235f;
        const RectF tile(0.5f, 0.5f, size - 1.0f, size - 1.0f);

        GraphicsPath path;
        {
            const float d = radius * 2.0f;
            path.AddArc(tile.X, tile.Y, d, d, 180.0f, 90.0f);
            path.AddArc(tile.GetRight() - d, tile.Y, d, d, 270.0f, 90.0f);
            path.AddArc(tile.GetRight() - d, tile.GetBottom() - d, d, d, 0.0f, 90.0f);
            path.AddArc(tile.X, tile.GetBottom() - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
        }

        LinearGradientBrush tile_brush(tile, Color(255, 0x59, 0x9B, 0xF7),
                                       Color(255, 0x21, 0x53, 0xBE), 90.0f);
        g.FillPath(&tile_brush, &path);

        // A sheen across the top half, fading out before the middle. Without it
        // the tile reads as a flat chip of colour.
        {
            const RectF sheen{tile.X, tile.Y, tile.Width, tile.Height * 0.55f};
            GraphicsPath sheen_path;
            const float d = radius * 2.0f;
            sheen_path.AddArc(sheen.X, sheen.Y, d, d, 180.0f, 90.0f);
            sheen_path.AddArc(sheen.GetRight() - d, sheen.Y, d, d, 270.0f, 90.0f);
            sheen_path.AddLine(sheen.GetRight(), sheen.GetBottom(), sheen.X, sheen.GetBottom());
            sheen_path.CloseFigure();
            LinearGradientBrush sheen_brush(sheen, Color(70, 0xFF, 0xFF, 0xFF),
                                            Color(0, 0xFF, 0xFF, 0xFF), 90.0f);
            g.FillPath(&sheen_brush, &sheen_path);
        }

        // A hairline rim, so the tile has an edge against a light background.
        {
            Pen rim(Color(60, 0x0B, 0x1B, 0x3A), std::max(1.0f, size * 0.012f));
            g.DrawPath(&rim, &path);
        }

        draw_mark(g, size, radius);
    }

    HBITMAP colour = nullptr;
    if (bitmap.GetHBITMAP(Color(0, 0, 0, 0), &colour) != Ok || colour == nullptr) {
        return nullptr;
    }

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    // CreateIconIndirect wants a mask even for an alpha icon; an empty one lets
    // the colour bitmap's alpha decide every pixel.
    info.hbmMask = CreateBitmap(size_px, size_px, 1, 1, nullptr);
    const HICON icon = CreateIconIndirect(&info);

    DeleteObject(colour);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return icon;
}

}  // namespace mag
