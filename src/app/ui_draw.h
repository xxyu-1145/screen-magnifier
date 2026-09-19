// app/ui_draw.h — the drawing layer the settings window is painted with.
//
// The first version drew its chrome with GDI's RoundRect and a one-pixel pen,
// which is a wireframe: hard aliased edges, flat fills, no depth. This layer
// uses GDI+ instead, so shapes are anti-aliased, fills can be gradients, and
// panels can carry a soft shadow — the things that make a surface read as
// rendered rather than sketched.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objidl.h>

#include <gdiplus.h>

#include <memory>

namespace mag::ui {

// Process-wide GDI+ lifetime. One instance, owned by the app host, created
// before any window is painted.
class GdiPlusScope {
public:
    GdiPlusScope();
    ~GdiPlusScope();

    GdiPlusScope(const GdiPlusScope&) = delete;
    GdiPlusScope& operator=(const GdiPlusScope&) = delete;

    bool ok() const noexcept { return started_; }

private:
    ULONG_PTR token_{0};
    bool started_{false};
};

// The palette and the type faces, resolved once per process.
struct Theme {
    Gdiplus::Color page_top{};
    Gdiplus::Color page_bottom{};
    Gdiplus::Color card{};
    Gdiplus::Color card_edge{};
    Gdiplus::Color separator{};

    Gdiplus::Color text{};
    Gdiplus::Color text_muted{};
    Gdiplus::Color accent{};
    Gdiplus::Color accent_dark{};
    Gdiplus::Color accent_soft{};
    Gdiplus::Color on_accent{};

    Gdiplus::Color control{};
    Gdiplus::Color control_edge{};
    Gdiplus::Color field{};
    Gdiplus::Color field_edge{};

    Gdiplus::FontFamily* body_family{nullptr};
    Gdiplus::FontFamily* symbol_family{nullptr};
    float body_size{10.0f};
    float title_size{15.0f};
    float label_size{9.0f};
};

// Resolved on first use; never null.
const Theme& theme();

// --- shapes ---------------------------------------------------------------

void round_rect_path(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r,
                     float radius) noexcept;

void fill_round(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                const Gdiplus::Color& colour);

// Vertical gradient fill, top to bottom.
void fill_round_vertical(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                         const Gdiplus::Color& top, const Gdiplus::Color& bottom);

void stroke_round(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                  const Gdiplus::Color& colour, float width);

// A soft shadow under a rounded rectangle, built from concentric strokes so it
// costs nothing beyond a few path fills and needs no blurred bitmap.
void drop_shadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, float spread,
                 BYTE alpha);

// --- text -----------------------------------------------------------------

enum TextFlags {
    TextLeft = 0,
    TextCenter = 1 << 0,
    TextRight = 1 << 1,
    TextVCenter = 1 << 2,
    TextEllipsis = 1 << 3,
    TextWrap = 1 << 4,
};

void draw_text(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::RectF& r,
               Gdiplus::Font& font, const Gdiplus::Color& colour, unsigned flags);

// A tick mark drawn as two strokes. Drawing it rather than setting a glyph
// keeps it crisp at any size and independent of the font's symbol coverage.
void draw_check(Gdiplus::Graphics& g, const Gdiplus::RectF& box, const Gdiplus::Color& colour,
                float width);

// Measures a string in the given font, for laying out by content rather than
// by guesswork.
Gdiplus::SizeF measure(Gdiplus::Graphics& g, const wchar_t* text, Gdiplus::Font& font);

}  // namespace mag::ui
