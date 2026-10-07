#include "gui/gdi_utils.h"

#include <algorithm>

namespace ir::gui {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

Gdiplus::Color argb(uint32_t c) {
    return Gdiplus::Color(
        static_cast<BYTE>((c >> 24) & 0xFF),
        static_cast<BYTE>((c >> 16) & 0xFF),
        static_cast<BYTE>((c >> 8) & 0xFF),
        static_cast<BYTE>(c & 0xFF));
}

void fill_round_rect(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& r,
    float radius,
    uint32_t fill_argb) {
    Gdiplus::GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(argb(fill_argb));
    g.FillPath(&brush, &path);
}

void draw_round_rect(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& r,
    float radius,
    uint32_t stroke_argb,
    float pen_w) {
    Gdiplus::GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::Pen pen(argb(stroke_argb), pen_w);
    g.DrawPath(&pen, &path);
}

void draw_text(
    Gdiplus::Graphics& g,
    const std::wstring& text,
    const Gdiplus::RectF& r,
    uint32_t color_argb,
    float size_pt,
    bool bold,
    Gdiplus::StringAlignment align_h,
    Gdiplus::StringAlignment align_v) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(
        &family,
        size_pt,
        bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
        Gdiplus::UnitPoint);
    Gdiplus::SolidBrush brush(argb(color_argb));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(align_h);
    fmt.SetLineAlignment(align_v);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    // Allow descenders (y, g, p) to paint outside the layout box — avoids clipped glyphs.
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
    g.DrawString(text.c_str(), -1, &font, r, &fmt, &brush);
}

bool point_in_rect(int x, int y, const RECT& r) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool point_in_rectf(int x, int y, const Gdiplus::RectF& r) {
    return static_cast<float>(x) >= r.X && static_cast<float>(x) < r.X + r.Width &&
           static_cast<float>(y) >= r.Y && static_cast<float>(y) < r.Y + r.Height;
}

void draw_brand_u(Gdiplus::Graphics& g, const Gdiplus::RectF& box, uint32_t color_argb) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    const float size_pt = box.Height * 0.58f;
    draw_text(
        g,
        L"U",
        box,
        color_argb,
        size_pt,
        true,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
}

void draw_brand_uo_ring(Gdiplus::Graphics& g, const Gdiplus::RectF& box, uint32_t color_argb) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const float pen_w = (std::max)(1.6f, box.Width * 0.032f);
    const float pad = pen_w * 0.5f + 1.0f;
    Gdiplus::RectF ring(box.X + pad, box.Y + pad, box.Width - pad * 2.0f, box.Height - pad * 2.0f);
    Gdiplus::Pen pen(argb(color_argb), pen_w);
    pen.SetAlignment(Gdiplus::PenAlignmentCenter);
    g.DrawEllipse(&pen, ring);

    const float size_pt = box.Height * 0.30f;
    draw_text(
        g,
        L"Uo",
        box,
        color_argb,
        size_pt,
        true,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
}

}  // namespace ir::gui
