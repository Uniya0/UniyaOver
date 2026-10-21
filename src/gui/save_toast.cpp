#include "gui/save_toast.h"

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ir::gui {
namespace {

constexpr UINT_PTR kTimerAnim = 1;
constexpr UINT kFrameMs = 15;

// Layout in 96-DPI pixels; scaled by the target monitor's DPI.
constexpr float kCanvasW = 360.0f;
constexpr float kCanvasH = 112.0f;
constexpr float kMargin = 18.0f;
constexpr float kPad = 18.0f;
constexpr float kRadius = 26.0f;
constexpr float kGap = 14.0f;
constexpr float kTitlePx = 19.0f;
constexpr float kSubtitlePx = 13.0f;

constexpr double kFadeOutMs = 420.0;
constexpr double kSavedTotalMs = 2300.0;
constexpr double kNoticeTotalMs = 2900.0;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

float clamp01(double v) {
    return static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
}
float ease_out_cubic(float x) {
    const float k = 1.0f - x;
    return 1.0f - k * k * k;
}
float ease_in_cubic(float x) {
    return x * x * x;
}
float ease_in_out_cubic(float x) {
    return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
}
float ease_out_back(float x) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float k = x - 1.0f;
    return 1.0f + c3 * k * k * k + c1 * k * k;
}

// The rest of the app is DPI-unaware; the toast opts into per-monitor DPI per call so it
// renders crisp and is positioned in physical pixels (same space as DXGI output rects).
class PerMonitorDpiScope {
public:
    PerMonitorDpiScope() {
        using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        static const Fn set_ctx = reinterpret_cast<Fn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
        set_ctx_ = set_ctx;
        if (!set_ctx_) {
            return;
        }
        prev_ = set_ctx_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!prev_) {
            prev_ = set_ctx_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        }
    }
    ~PerMonitorDpiScope() {
        if (set_ctx_ && prev_) {
            set_ctx_(prev_);
        }
    }
    PerMonitorDpiScope(const PerMonitorDpiScope&) = delete;
    PerMonitorDpiScope& operator=(const PerMonitorDpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT (WINAPI* set_ctx_)(DPI_AWARENESS_CONTEXT) = nullptr;
    DPI_AWARENESS_CONTEXT prev_ = nullptr;
};

UINT monitor_dpi(HMONITOR monitor) {
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const Fn get_dpi = [] {
        HMODULE shcore = LoadLibraryExW(L"Shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return shcore ? reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    }();
    UINT x = 96;
    UINT y = 96;
    if (get_dpi && SUCCEEDED(get_dpi(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x > 0) {
        return x;
    }
    return 96;
}

bool is_bottom(SaveToast::Position p) {
    return p == SaveToast::Position::BottomLeft || p == SaveToast::Position::BottomCenter ||
           p == SaveToast::Position::BottomRight;
}
bool is_left(SaveToast::Position p) {
    return p == SaveToast::Position::TopLeft || p == SaveToast::Position::BottomLeft;
}
bool is_center(SaveToast::Position p) {
    return p == SaveToast::Position::TopCenter || p == SaveToast::Position::BottomCenter;
}

// Borderless/fullscreen apps hide the taskbar, so the toast may use the whole monitor;
// otherwise stay inside the work area to avoid overlapping the taskbar.
RECT placement_area(const MONITORINFO& mi) {
    HWND fg = GetForegroundWindow();
    RECT wr{};
    if (fg && GetWindowRect(fg, &wr) && EqualRect(&wr, &mi.rcMonitor)) {
        wchar_t cls[32] = {};
        GetClassNameW(fg, cls, 32);
        if (lstrcmpW(cls, L"Progman") != 0 && lstrcmpW(cls, L"WorkerW") != 0) {
            return mi.rcMonitor;
        }
    }
    return mi.rcWork;
}

std::unique_ptr<Gdiplus::FontFamily> make_family() {
    for (const wchar_t* name : {L"Segoe UI Variable Display", L"Segoe UI"}) {
        auto family = std::make_unique<Gdiplus::FontFamily>(name);
        if (family->GetLastStatus() == Gdiplus::Ok) {
            return family;
        }
    }
    return std::unique_ptr<Gdiplus::FontFamily>(Gdiplus::FontFamily::GenericSansSerif()->Clone());
}

void draw_partial_polyline(
    Gdiplus::Graphics& g,
    Gdiplus::Pen& pen,
    const Gdiplus::PointF* pts,
    int count,
    float t) {
    float total = 0.0f;
    for (int i = 0; i + 1 < count; ++i) {
        total += std::hypot(pts[i + 1].X - pts[i].X, pts[i + 1].Y - pts[i].Y);
    }
    float remaining = total * t;
    for (int i = 0; i + 1 < count && remaining > 0.0f; ++i) {
        const float seg = std::hypot(pts[i + 1].X - pts[i].X, pts[i + 1].Y - pts[i].Y);
        const float k = (std::min)(1.0f, remaining / seg);
        const Gdiplus::PointF end(
            pts[i].X + (pts[i + 1].X - pts[i].X) * k, pts[i].Y + (pts[i + 1].Y - pts[i].Y) * k);
        g.DrawLine(&pen, pts[i], end);
        remaining -= seg;
    }
}

void draw_glow_text(
    Gdiplus::Graphics& g,
    Gdiplus::GraphicsPath& path,
    uint32_t rgb,
    float alpha,
    float scale) {
    struct Halo {
        float width;
        float alpha;
    };
    constexpr Halo halos[] = {{8.0f, 18.0f}, {5.0f, 30.0f}, {2.6f, 60.0f}};
    for (const Halo& h : halos) {
        Gdiplus::Pen pen(
            Gdiplus::Color(static_cast<BYTE>(h.alpha * alpha), 0, 0, 0), h.width * scale);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&pen, &path);
    }
    Gdiplus::SolidBrush fill(Gdiplus::Color(
        static_cast<BYTE>(255.0f * alpha),
        static_cast<BYTE>((rgb >> 16) & 0xFF),
        static_cast<BYTE>((rgb >> 8) & 0xFF),
        static_cast<BYTE>(rgb & 0xFF)));
    g.FillPath(&fill, &path);
}

}  // namespace

SaveToast::~SaveToast() {
    destroy();
}

bool SaveToast::create(HINSTANCE instance) {
    if (hwnd_) {
        return true;
    }
    const wchar_t* class_name = L"UniyaOverSaveToast";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = static_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ir::log::error("SaveToast: RegisterClassExW failed");
        return false;
    }

    PerMonitorDpiScope dpi;
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        class_name,
        L"",
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        this);
    if (!hwnd_) {
        ir::log::error("SaveToast: CreateWindowExW failed, error=" + std::to_string(GetLastError()));
        return false;
    }
    // Keep the toast out of Desktop Duplication so it never lands in later clips.
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        ir::log::warn("SaveToast: capture exclusion unavailable (needs Windows 10 2004+)");
    }
    return true;
}

void SaveToast::destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kTimerAnim);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    release_surface();
    family_.reset();
    visible_ = false;
}

void SaveToast::set_style(Position position, float size) {
    position_ = position;
    size_ = (std::max)(0.5f, (std::min)(2.0f, size));
}

void SaveToast::show(
    Kind kind,
    const std::wstring& title,
    const std::wstring& subtitle,
    const RECT& monitor_hint) {
    if (!hwnd_) {
        return;
    }
    PerMonitorDpiScope dpi;
    HMONITOR monitor = nullptr;
    if (monitor_hint.right > monitor_hint.left && monitor_hint.bottom > monitor_hint.top) {
        monitor = MonitorFromRect(&monitor_hint, MONITOR_DEFAULTTONEAREST);
    }
    show_on(kind, title, subtitle, monitor);
}

void SaveToast::show(
    Kind kind,
    const std::wstring& title,
    const std::wstring& subtitle,
    HMONITOR monitor) {
    if (!hwnd_) {
        return;
    }
    PerMonitorDpiScope dpi;
    show_on(kind, title, subtitle, monitor);
}

void SaveToast::show_on(
    Kind kind,
    const std::wstring& title,
    const std::wstring& subtitle,
    HMONITOR monitor) {
    if (!monitor) {
        monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return;
    }

    scale_ = static_cast<float>(monitor_dpi(monitor)) / 96.0f * size_;
    const int w = static_cast<int>(std::lround(kCanvasW * scale_));
    const int h = static_cast<int>(std::lround(kCanvasH * scale_));
    if (!ensure_surface(w, h)) {
        return;
    }
    const RECT area = placement_area(mi);
    const int margin = static_cast<int>(std::lround(kMargin * scale_));
    if (is_left(position_)) {
        pos_.x = area.left + margin;
    } else if (is_center(position_)) {
        pos_.x = area.left + (area.right - area.left - w) / 2;
    } else {
        pos_.x = area.right - w - margin;
    }
    pos_.y = is_bottom(position_) ? area.bottom - h - margin : area.top + margin;

    kind_ = kind;
    title_ = title;
    subtitle_ = subtitle;
    start_ms_ = GetTickCount64();
    visible_ = true;

    render_frame();
    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(hwnd_, kTimerAnim, kFrameMs, nullptr);
}

void SaveToast::hide() {
    KillTimer(hwnd_, kTimerAnim);
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

bool SaveToast::ensure_surface(int w, int h) {
    if (dib_ && w == surface_w_ && h == surface_h_) {
        return true;
    }
    release_surface();

    mem_dc_ = CreateCompatibleDC(nullptr);
    if (!mem_dc_) {
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_ || !bits_) {
        release_surface();
        return false;
    }
    old_bitmap_ = SelectObject(mem_dc_, dib_);
    surface_w_ = w;
    surface_h_ = h;
    return true;
}

void SaveToast::release_surface() {
    if (mem_dc_ && old_bitmap_) {
        SelectObject(mem_dc_, old_bitmap_);
    }
    if (dib_) {
        DeleteObject(dib_);
    }
    if (mem_dc_) {
        DeleteDC(mem_dc_);
    }
    mem_dc_ = nullptr;
    dib_ = nullptr;
    old_bitmap_ = nullptr;
    bits_ = nullptr;
    surface_w_ = 0;
    surface_h_ = 0;
}

void SaveToast::render_frame() {
    if (!visible_ || !bits_) {
        return;
    }
    const double t = static_cast<double>(GetTickCount64() - start_ms_);
    const double total = kind_ == Kind::Saved ? kSavedTotalMs : kNoticeTotalMs;
    if (t >= total) {
        hide();
        return;
    }

    const float s = scale_;
    const float appear = ease_out_cubic(clamp01(t / 220.0));
    const float pop = ease_out_back(clamp01(t / 340.0));
    const float ring = ease_in_out_cubic(clamp01((t - 60.0) / 460.0));
    const float mark = ease_out_cubic(clamp01((t - 300.0) / 320.0));
    const float burst = clamp01((t - 620.0) / 520.0);
    const float text_in = ease_out_cubic(clamp01((t - 200.0) / 340.0));
    const float leave = ease_in_cubic(clamp01((t - (total - kFadeOutMs)) / kFadeOutMs));
    const float opacity = appear * (1.0f - leave);

    {
        Gdiplus::Bitmap canvas(
            surface_w_,
            surface_h_,
            surface_w_ * 4,
            PixelFormat32bppPARGB,
            static_cast<BYTE*>(bits_));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        if (!family_) {
            family_ = make_family();
        }
        Gdiplus::FontFamily* family = family_.get();
        std::unique_ptr<Gdiplus::StringFormat> fmt(
            Gdiplus::StringFormat::GenericTypographic()->Clone());

        const float title_px = kTitlePx * s;
        const float subtitle_px = kSubtitlePx * s;
        Gdiplus::GraphicsPath title_path;
        title_path.AddString(
            title_.c_str(),
            static_cast<INT>(title_.size()),
            family,
            Gdiplus::FontStyleBold,
            title_px,
            Gdiplus::PointF(0.0f, 0.0f),
            fmt.get());
        Gdiplus::GraphicsPath subtitle_path;
        subtitle_path.AddString(
            subtitle_.c_str(),
            static_cast<INT>(subtitle_.size()),
            family,
            Gdiplus::FontStyleRegular,
            subtitle_px,
            Gdiplus::PointF(0.0f, title_px * 1.32f),
            fmt.get());

        Gdiplus::RectF tb{};
        Gdiplus::RectF sb{};
        title_path.GetBounds(&tb);
        subtitle_path.GetBounds(&sb);
        Gdiplus::RectF text_bounds{};
        Gdiplus::RectF::Union(text_bounds, tb, sb);

        const float radius = kRadius * s;
        const float content_w = radius * 2.0f + kGap * s + text_bounds.Width;
        float left = (std::max)(kPad * s, surface_w_ - kPad * s - content_w);
        if (is_left(position_)) {
            left = kPad * s;
        } else if (is_center(position_)) {
            left = (std::max)(kPad * s, (surface_w_ - content_w) * 0.5f);
        }
        const float cx = left + radius;
        const float cy = surface_h_ * 0.5f;

        // Soft dark halo so the white mark stays readable over bright frames.
        {
            const float hr = radius * 2.1f;
            Gdiplus::GraphicsPath halo;
            halo.AddEllipse(cx - hr, cy - hr, hr * 2.0f, hr * 2.0f);
            Gdiplus::PathGradientBrush brush(&halo);
            brush.SetCenterColor(Gdiplus::Color(120, 0, 0, 0));
            Gdiplus::Color surround(0, 0, 0, 0);
            INT n = 1;
            brush.SetSurroundColors(&surround, &n);
            g.FillPath(&brush, &halo);
        }

        const float r = radius * (0.62f + 0.38f * pop);
        Gdiplus::SolidBrush disc(Gdiplus::Color(110, 10, 10, 10));
        g.FillEllipse(&disc, cx - r, cy - r, r * 2.0f, r * 2.0f);

        Gdiplus::Pen track(Gdiplus::Color(46, 255, 255, 255), 2.2f * s);
        g.DrawEllipse(&track, cx - r, cy - r, r * 2.0f, r * 2.0f);
        if (ring > 0.0f) {
            Gdiplus::Pen arc(Gdiplus::Color(255, 255, 255, 255), 2.4f * s);
            arc.SetStartCap(Gdiplus::LineCapRound);
            arc.SetEndCap(Gdiplus::LineCapRound);
            g.DrawArc(&arc, cx - r, cy - r, r * 2.0f, r * 2.0f, -90.0f, 360.0f * ring);
        }

        if (kind_ == Kind::Saved && burst > 0.0f && burst < 1.0f) {
            const float br = r * (1.0f + 0.75f * ease_out_cubic(burst));
            Gdiplus::Pen wave(
                Gdiplus::Color(static_cast<BYTE>(150.0f * (1.0f - burst)), 255, 255, 255),
                2.0f * s * (1.0f - 0.5f * burst));
            g.DrawEllipse(&wave, cx - br, cy - br, br * 2.0f, br * 2.0f);
        }

        if (mark > 0.0f) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), 3.4f * s);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            if (kind_ == Kind::Saved) {
                const Gdiplus::PointF check[] = {
                    {cx - 0.40f * r, cy + 0.02f * r},
                    {cx - 0.12f * r, cy + 0.30f * r},
                    {cx + 0.42f * r, cy - 0.30f * r},
                };
                draw_partial_polyline(g, pen, check, 3, mark);
            } else if (kind_ == Kind::Warning) {
                const Gdiplus::PointF bar[] = {{cx, cy - 0.42f * r}, {cx, cy + 0.10f * r}};
                draw_partial_polyline(g, pen, bar, 2, (std::min)(1.0f, mark * 1.25f));
                if (mark > 0.8f) {
                    const float dot = 2.1f * s;
                    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
                    g.FillEllipse(&white, cx - dot, cy + 0.36f * r - dot, dot * 2.0f, dot * 2.0f);
                }
            } else {
                const float d = 0.30f * r;
                const Gdiplus::PointF a[] = {{cx - d, cy - d}, {cx + d, cy + d}};
                const Gdiplus::PointF b[] = {{cx + d, cy - d}, {cx - d, cy + d}};
                draw_partial_polyline(g, pen, a, 2, (std::min)(1.0f, mark * 2.0f));
                draw_partial_polyline(g, pen, b, 2, clamp01(mark * 2.0f - 1.0f));
            }
        }

        if (text_in > 0.0f) {
            const float text_x = left + radius * 2.0f + kGap * s + (1.0f - text_in) * 12.0f * s;
            Gdiplus::Matrix move;
            move.Translate(
                text_x - text_bounds.X, cy - (text_bounds.Y + text_bounds.Height * 0.5f));
            title_path.Transform(&move);
            subtitle_path.Transform(&move);
            draw_glow_text(g, title_path, 0xFFFFFF, text_in, s);
            draw_glow_text(g, subtitle_path, 0xD4D4D4, text_in * 0.95f, s);
        }
        g.Flush(Gdiplus::FlushIntentionSync);
    }

    PerMonitorDpiScope dpi;
    HDC screen = GetDC(nullptr);
    SIZE size{surface_w_, surface_h_};
    POINT src{0, 0};
    // Drift toward the nearest screen edge while fading out.
    const LONG drift = static_cast<LONG>(std::lround(8.0f * s * leave));
    POINT dst{pos_.x, is_bottom(position_) ? pos_.y + drift : pos_.y - drift};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(255.0f * opacity), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK SaveToast::static_wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    SaveToast* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<SaveToast*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SaveToast*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self && self->hwnd_ == hwnd) {
        return self->wnd_proc(msg, w_param, l_param);
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

LRESULT SaveToast::wnd_proc(UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
        case WM_TIMER:
            if (w_param == kTimerAnim) {
                render_frame();
            }
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, w_param, l_param);
}

}  // namespace ir::gui
