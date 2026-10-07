#include "gui/region_selector.h"

#include "util/log.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ir::gui {
namespace {

struct SelectorState {
    HWND hwnd = nullptr;
    RECT monitor{};
    int capture_w = 0;
    int capture_h = 0;
    int width = 0;
    int height = 0;
    bool dragging = false;
    bool done = false;
    bool ok = false;
    POINT start{};
    POINT cur{};
    RECT result{};
    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_bmp = nullptr;
    void* bits = nullptr;
};

SelectorState* g_sel = nullptr;

RECT normalize_rect(POINT a, POINT b) {
    RECT r{};
    r.left = (std::min)(a.x, b.x);
    r.top = (std::min)(a.y, b.y);
    r.right = (std::max)(a.x, b.x);
    r.bottom = (std::max)(a.y, b.y);
    return r;
}

RECT client_to_capture(const SelectorState& s, RECT client) {
    const int mon_w = s.monitor.right - s.monitor.left;
    const int mon_h = s.monitor.bottom - s.monitor.top;
    if (mon_w <= 0 || mon_h <= 0 || s.capture_w <= 0 || s.capture_h <= 0) {
        return {};
    }

    auto map_x = [&](int x) -> int {
        return static_cast<int>(std::lround(
            static_cast<double>(x) * static_cast<double>(s.capture_w) / static_cast<double>(mon_w)));
    };
    auto map_y = [&](int y) -> int {
        return static_cast<int>(std::lround(
            static_cast<double>(y) * static_cast<double>(s.capture_h) / static_cast<double>(mon_h)));
    };

    RECT out{};
    out.left = map_x(client.left);
    out.top = map_y(client.top);
    out.right = map_x(client.right);
    out.bottom = map_y(client.bottom);

    out.left = (std::max)(0, static_cast<int>(out.left)) & ~1;
    out.top = (std::max)(0, static_cast<int>(out.top)) & ~1;
    out.right = (std::min)(s.capture_w, static_cast<int>(out.right)) & ~1;
    out.bottom = (std::min)(s.capture_h, static_cast<int>(out.bottom)) & ~1;
    if (out.right - out.left < 32) {
        out.right = (std::min)(s.capture_w, static_cast<int>(out.left) + 32) & ~1;
    }
    if (out.bottom - out.top < 32) {
        out.bottom = (std::min)(s.capture_h, static_cast<int>(out.top) + 32) & ~1;
    }
    return out;
}

void destroy_dib(SelectorState& s) {
    if (s.mem_dc && s.old_bmp) {
        SelectObject(s.mem_dc, s.old_bmp);
        s.old_bmp = nullptr;
    }
    if (s.dib) {
        DeleteObject(s.dib);
        s.dib = nullptr;
    }
    if (s.mem_dc) {
        DeleteDC(s.mem_dc);
        s.mem_dc = nullptr;
    }
    s.bits = nullptr;
}

bool ensure_dib(SelectorState& s) {
    if (s.dib && s.bits) {
        return true;
    }
    destroy_dib(s);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = s.width;
    bmi.bmiHeader.biHeight = -s.height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    s.mem_dc = CreateCompatibleDC(nullptr);
    s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
    if (!s.mem_dc || !s.dib || !s.bits) {
        destroy_dib(s);
        return false;
    }
    s.old_bmp = SelectObject(s.mem_dc, s.dib);
    return true;
}

void put_pixel(SelectorState& s, int x, int y, BYTE a, BYTE r, BYTE g, BYTE b) {
    if (x < 0 || y < 0 || x >= s.width || y >= s.height || !s.bits) {
        return;
    }
    auto* px = static_cast<BYTE*>(s.bits) + (static_cast<size_t>(y) * s.width + x) * 4;
    // Premultiplied BGRA for UpdateLayeredWindow
    px[0] = static_cast<BYTE>((b * a) / 255);
    px[1] = static_cast<BYTE>((g * a) / 255);
    px[2] = static_cast<BYTE>((r * a) / 255);
    px[3] = a;
}

void fill_rect_alpha(SelectorState& s, int x0, int y0, int x1, int y1, BYTE a, BYTE r, BYTE g, BYTE b) {
    x0 = (std::max)(0, x0);
    y0 = (std::max)(0, y0);
    x1 = (std::min)(s.width, x1);
    y1 = (std::min)(s.height, y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            put_pixel(s, x, y, a, r, g, b);
        }
    }
}

void draw_frame(SelectorState& s, const RECT& sel, BYTE a, BYTE r, BYTE g, BYTE b) {
    const int x0 = sel.left;
    const int y0 = sel.top;
    const int x1 = sel.right;
    const int y1 = sel.bottom;
    for (int x = x0; x < x1; ++x) {
        put_pixel(s, x, y0, a, r, g, b);
        put_pixel(s, x, y0 + 1, a, r, g, b);
        put_pixel(s, x, y1 - 1, a, r, g, b);
        put_pixel(s, x, y1 - 2, a, r, g, b);
    }
    for (int y = y0; y < y1; ++y) {
        put_pixel(s, x0, y, a, r, g, b);
        put_pixel(s, x0 + 1, y, a, r, g, b);
        put_pixel(s, x1 - 1, y, a, r, g, b);
        put_pixel(s, x1 - 2, y, a, r, g, b);
    }
}

void update_overlay(HWND hwnd) {
    if (!g_sel || !ensure_dib(*g_sel)) {
        return;
    }
    SelectorState& s = *g_sel;
    // Dim everything (~40% black), hole = fully transparent over selection.
    std::memset(s.bits, 0, static_cast<size_t>(s.width) * static_cast<size_t>(s.height) * 4);
    fill_rect_alpha(s, 0, 0, s.width, s.height, 110, 0, 0, 0);

    const bool has_sel = s.dragging || (s.cur.x != s.start.x || s.cur.y != s.start.y);
    if (has_sel) {
        RECT sel = normalize_rect(s.start, s.cur);
        if (sel.right - sel.left > 2 && sel.bottom - sel.top > 2) {
            fill_rect_alpha(s, sel.left, sel.top, sel.right, sel.bottom, 0, 0, 0, 0);
            draw_frame(s, sel, 230, 240, 240, 240);
        }
    }

    POINT pt_src{0, 0};
    SIZE size{s.width, s.height};
    POINT pt_dst{s.monitor.left, s.monitor.top};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, nullptr, &pt_dst, &size, s.mem_dc, &pt_src, 0, &blend, ULW_ALPHA);
}

void commit_selection() {
    if (!g_sel) {
        return;
    }
    const RECT client = normalize_rect(g_sel->start, g_sel->cur);
    if (client.right - client.left < 8 || client.bottom - client.top < 8) {
        return;
    }
    g_sel->result = client_to_capture(*g_sel, client);
    if (g_sel->result.right - g_sel->result.left >= 32 &&
        g_sel->result.bottom - g_sel->result.top >= 32) {
        g_sel->ok = true;
        g_sel->done = true;
        PostMessageW(g_sel->hwnd, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK selector_wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (g_sel) {
            g_sel->dragging = true;
            g_sel->start = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            g_sel->cur = g_sel->start;
            SetCapture(hwnd);
            update_overlay(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_sel && g_sel->dragging) {
            g_sel->cur = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            update_overlay(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_sel && g_sel->dragging) {
            g_sel->dragging = false;
            g_sel->cur = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ReleaseCapture();
            update_overlay(hwnd);
            commit_selection();  // accept on mouse release
        }
        return 0;
    case WM_KEYDOWN:
        if (w_param == VK_ESCAPE) {
            if (g_sel) {
                g_sel->ok = false;
                g_sel->done = true;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        update_overlay(hwnd);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (g_sel) {
            destroy_dib(*g_sel);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

}  // namespace

bool pick_screen_region(
    HWND owner,
    const RECT& monitor_screen,
    int capture_w,
    int capture_h,
    RECT* out_crop) {
    if (!out_crop || capture_w < 32 || capture_h < 32) {
        return false;
    }

    const int mon_w = monitor_screen.right - monitor_screen.left;
    const int mon_h = monitor_screen.bottom - monitor_screen.top;
    if (mon_w < 32 || mon_h < 32) {
        ir::log::error("Region selector: invalid monitor rect");
        return false;
    }

    SelectorState state;
    state.monitor = monitor_screen;
    state.capture_w = capture_w;
    state.capture_h = capture_h;
    state.width = mon_w;
    state.height = mon_h;
    g_sel = &state;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = selector_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = L"UniyaOverRegionSelector";
    RegisterClassExW(&wc);

    state.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"UniyaOverRegionSelector",
        L"UniyaOver region",
        WS_POPUP,
        monitor_screen.left,
        monitor_screen.top,
        mon_w,
        mon_h,
        owner,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!state.hwnd) {
        g_sel = nullptr;
        ir::log::error("Region selector CreateWindowEx failed");
        return false;
    }

    ShowWindow(state.hwnd, SW_SHOW);
    update_overlay(state.hwnd);
    SetForegroundWindow(state.hwnd);

    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(state.hwnd)) {
        DestroyWindow(state.hwnd);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    destroy_dib(state);
    g_sel = nullptr;
    if (!state.ok) {
        ir::log::info("Region select cancelled");
        return false;
    }
    *out_crop = state.result;
    ir::log::info(
        "Region selected: " + std::to_string(out_crop->left) + "," +
        std::to_string(out_crop->top) + " " +
        std::to_string(out_crop->right - out_crop->left) + "x" +
        std::to_string(out_crop->bottom - out_crop->top));
    return true;
}

}  // namespace ir::gui
