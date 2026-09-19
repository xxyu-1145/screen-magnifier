// app/ui_draw.cpp — the GDI+ drawing layer.

#include "app/ui_draw.h"

#include <algorithm>
#include <cmath>

namespace mag::ui {
namespace {

// One family for text and one for symbols; resolved by asking GDI+ whether the
// family exists rather than assuming a Windows version has it.
Gdiplus::FontFamily* pick_family(const wchar_t* const* candidates, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        auto* family = new Gdiplus::FontFamily(candidates[i]);
        if (family->IsAvailable()) return family;
        delete family;
    }
    return nullptr;
}

Theme build_theme() {
    Theme t;
    // A very slight vertical gradient on the page: enough to read as a surface
    // rather than a flat fill, not enough to notice as a gradient.
    t.page_top = Gdiplus::Color(255, 0xF7, 0xF8, 0xFB);
    t.page_bottom = Gdiplus::Color(255, 0xEC, 0xEF, 0xF5);
    t.card = Gdiplus::Color(255, 0xFF, 0xFF, 0xFF);
    t.card_edge = Gdiplus::Color(255, 0xE6, 0xEA, 0xF1);
    t.separator = Gdiplus::Color(255, 0xE9, 0xEC, 0xF2);

    t.text = Gdiplus::Color(255, 0x1B, 0x20, 0x2B);
    t.text_muted = Gdiplus::Color(255, 0x68, 0x71, 0x82);
    t.accent = Gdiplus::Color(255, 0x3B, 0x7B, 0xF0);
    t.accent_dark = Gdiplus::Color(255, 0x2A, 0x5C, 0xC4);
    t.accent_soft = Gdiplus::Color(255, 0xE9, 0xF0, 0xFE);
    t.on_accent = Gdiplus::Color(255, 0xFF, 0xFF, 0xFF);

    t.control = Gdiplus::Color(255, 0xFF, 0xFF, 0xFF);
    t.control_edge = Gdiplus::Color(255, 0xDA, 0xDF, 0xE8);
    t.field = Gdiplus::Color(255, 0xFF, 0xFF, 0xFF);
    t.field_edge = Gdiplus::Color(255, 0xD5, 0xDB, 0xE5);

    static const wchar_t* const kText[] = {L"Microsoft YaHei UI", L"Microsoft YaHei",
                                           L"Segoe UI", L"Tahoma"};
    static const wchar_t* const kSymbol[] = {L"Segoe UI Symbol", L"Segoe UI", L"Tahoma"};
    t.body_family = pick_family(kText, std::size(kText));
    t.symbol_family = pick_family(kSymbol, std::size(kSymbol));
    return t;
}

}  // namespace

GdiPlusScope::GdiPlusScope() {
    Gdiplus::GdiplusStartupInput input;
    started_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
}

GdiPlusScope::~GdiPlusScope() {
    if (started_) Gdiplus::GdiplusShutdown(token_);
}

const Theme& theme() {
    static const Theme instance = build_theme();
    return instance;
}

void round_rect_path(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r,
                     float radius) noexcept {
    const float max_radius = std::min(r.Width, r.Height) * 0.5f;
    const float rad = std::max(0.0f, std::min(radius, max_radius));
    if (rad <= 0.01f) {
        path.AddRectangle(r);
        return;
    }
    const float d = rad * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void fill_round(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                const Gdiplus::Color& colour) {
    Gdiplus::GraphicsPath path;
    round_rect_path(path, r, radius);
    Gdiplus::SolidBrush brush(colour);
    g.FillPath(&brush, &path);
}

void fill_round_vertical(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                         const Gdiplus::Color& top, const Gdiplus::Color& bottom) {
    Gdiplus::GraphicsPath path;
    round_rect_path(path, r, radius);
    Gdiplus::LinearGradientBrush brush(r, top, bottom, 90.0f);
    g.FillPath(&brush, &path);
}

void stroke_round(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                  const Gdiplus::Color& colour, float width) {
    Gdiplus::GraphicsPath path;
    round_rect_path(path, r, radius);
    Gdiplus::Pen pen(colour, width);
    g.DrawPath(&pen, &path);
}

void drop_shadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, float spread,
                 BYTE alpha) {
    // Concentric rings of decreasing alpha. A real Gaussian blur would need an
    // offscreen bitmap and a convolution; this reads the same at these sizes.
    const int steps = 4;
    for (int i = steps; i >= 1; --i) {
        const float grow = spread * static_cast<float>(i) / static_cast<float>(steps);
        const BYTE a = static_cast<BYTE>(
            alpha * (1.0f - static_cast<float>(i) / static_cast<float>(steps + 1)) / steps * 2.4f);
        if (a == 0) continue;
        const Gdiplus::RectF ring{r.X - grow, r.Y - grow + 1.0f, r.Width + 2 * grow,
                                  r.Height + 2 * grow};
        Gdiplus::GraphicsPath path;
        round_rect_path(path, ring, radius + grow);
        Gdiplus::SolidBrush brush(Gdiplus::Color(a, 0x1B, 0x24, 0x38));
        g.FillPath(&brush, &path);
    }
}

void draw_text(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::RectF& r,
               Gdiplus::Font& font, const Gdiplus::Color& colour, unsigned flags) {
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

    if (flags & TextCenter) {
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
    } else if (flags & TextRight) {
        format.SetAlignment(Gdiplus::StringAlignmentFar);
    } else {
        format.SetAlignment(Gdiplus::StringAlignmentNear);
    }
    if (flags & TextVCenter) {
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    } else {
        format.SetLineAlignment(Gdiplus::StringAlignmentNear);
    }

    Gdiplus::SolidBrush brush(colour);
    g.DrawString(text, -1, &font, r, &format, &brush);
}

void draw_check(Gdiplus::Graphics& g, const Gdiplus::RectF& box, const Gdiplus::Color& colour,
                float width) {
    Gdiplus::Pen pen(colour, width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF points[3] = {
        {box.X + box.Width * 0.22f, box.Y + box.Height * 0.52f},
        {box.X + box.Width * 0.42f, box.Y + box.Height * 0.72f},
        {box.X + box.Width * 0.80f, box.Y + box.Height * 0.28f},
    };
    g.DrawLines(&pen, points, 3);
}

Gdiplus::SizeF measure(Gdiplus::Graphics& g, const wchar_t* text, Gdiplus::Font& font) {
    Gdiplus::RectF bounds;
    g.MeasureString(text, -1, &font, Gdiplus::PointF(0.0f, 0.0f), &bounds);
    return Gdiplus::SizeF(bounds.Width, bounds.Height);
}

}  // namespace mag::ui
