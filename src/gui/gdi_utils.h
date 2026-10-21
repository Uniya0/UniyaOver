#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <string>

namespace ir::gui {

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

Gdiplus::Color argb(uint32_t c);
// Per-channel lerp of two ARGB colors, t in [0, 1].
uint32_t mix_argb(uint32_t a, uint32_t b, float t);
uint32_t with_alpha(uint32_t c, float alpha);

// Frees cached fonts; call before GdiplusShutdown.
void release_gdi_cache();

void fill_round_rect(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& r,
    float radius,
    uint32_t fill_argb);

void draw_round_rect(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& r,
    float radius,
    uint32_t stroke_argb,
    float pen_w = 1.0f);

void draw_text(
    Gdiplus::Graphics& g,
    const std::wstring& text,
    const Gdiplus::RectF& r,
    uint32_t color_argb,
    float size_pt,
    bool bold = false,
    Gdiplus::StringAlignment align_h = Gdiplus::StringAlignmentNear,
    Gdiplus::StringAlignment align_v = Gdiplus::StringAlignmentNear);

bool point_in_rect(int x, int y, const RECT& r);
bool point_in_rectf(int x, int y, const Gdiplus::RectF& r);

// Sidebar: compact «U». Home: larger «Uo» inside a thin ring.
void draw_brand_u(Gdiplus::Graphics& g, const Gdiplus::RectF& box, uint32_t color_argb);
void draw_brand_uo_ring(Gdiplus::Graphics& g, const Gdiplus::RectF& box, uint32_t color_argb);

}  // namespace ir::gui
