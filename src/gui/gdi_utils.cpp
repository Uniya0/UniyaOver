#include "gui/gdi_utils.h"

#include <algorithm>
#include <unordered_map>

namespace ir::gui {
namespace {

// Heap-owned and freed only via release_gdi_cache(): a static destructor would run after
// GdiplusShutdown and crash on exit.
struct FontCache {
    Gdiplus::FontFamily* family = nullptr;
    std::unordered_map<uint32_t, Gdiplus::Font*> fonts;
};
FontCache* g_font_cache = nullptr;

Gdiplus::Font* cached_font(float size_pt, bool bold) {
    if (!g_font_cache) {
        g_font_cache = new FontCache();
        g_font_cache->family = new Gdiplus::FontFamily(L"Segoe UI");
    }
    const uint32_t key = (static_cast<uint32_t>(size_pt * 100.0f + 0.5f) << 1) | (bold ? 1u : 0u);
    auto it = g_font_cache->fonts.find(key);
    if (it != g_font_cache->fonts.end()) {
        return it->second;
    }
    auto* font = new Gdiplus::Font(
        g_font_cache->family,
        size_pt,
        bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
        Gdiplus::UnitPoint);
    g_font_cache->fonts.emplace(key, font);
    return font;
}

}  // namespace

void release_gdi_cache() {
    if (!g_font_cache) {
        return;
    }
    for (auto& [key, font] : g_font_cache->fonts) {
        delete font;
    }
    delete g_font_cache->family;
    delete g_font_cache;
    g_font_cache = nullptr;
}

uint32_t mix_argb(uint32_t a, uint32_t b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

uint32_t with_alpha(uint32_t c, float alpha) {
    const float a = static_cast<float>((c >> 24) & 0xFF) * (std::max)(0.0f, (std::min)(1.0f, alpha));
    return (c & 0x00FFFFFFu) | (static_cast<uint32_t>(a + 0.5f) << 24);
}

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
    Gdiplus::Font* font = cached_font(size_pt, bold);
    Gdiplus::SolidBrush brush(argb(color_argb));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(align_h);
    fmt.SetLineAlignment(align_v);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    // Allow descenders (y, g, p) to paint outside the layout box — avoids clipped glyphs.
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
    g.DrawString(text.c_str(), -1, font, r, &fmt, &brush);
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
