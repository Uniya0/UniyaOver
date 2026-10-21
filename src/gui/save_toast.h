#pragma once

#include "gui/gdi_utils.h"

#include <memory>
#include <string>

namespace ir::gui {

// Click-through, capture-excluded overlay shown on the captured monitor after a save attempt.
class SaveToast {
public:
    enum class Kind { Saved, Warning, Error };
    // Order matches AppSettings::toast_position.
    enum class Position { TopLeft, TopCenter, TopRight, BottomLeft, BottomCenter, BottomRight };

    SaveToast() = default;
    ~SaveToast();

    SaveToast(const SaveToast&) = delete;
    SaveToast& operator=(const SaveToast&) = delete;

    bool create(HINSTANCE instance);
    // Must run before GdiplusShutdown (releases GDI+ objects).
    void destroy();

    // size: multiplier on top of monitor DPI (1.0 = default look).
    void set_style(Position position, float size);

    // monitor_hint: desktop rect of the captured output; empty rect = foreground window's monitor.
    void show(
        Kind kind,
        const std::wstring& title,
        const std::wstring& subtitle,
        const RECT& monitor_hint);
    void show(
        Kind kind,
        const std::wstring& title,
        const std::wstring& subtitle,
        HMONITOR monitor);

private:
    void show_on(Kind kind, const std::wstring& title, const std::wstring& subtitle, HMONITOR monitor);
    static LRESULT CALLBACK static_wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT wnd_proc(UINT msg, WPARAM w_param, LPARAM l_param);

    bool ensure_surface(int w, int h);
    void release_surface();
    void render_frame();
    void hide();

    HWND hwnd_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    void* bits_ = nullptr;
    int surface_w_ = 0;
    int surface_h_ = 0;
    std::unique_ptr<Gdiplus::FontFamily> family_;

    Position position_ = Position::TopRight;
    float size_ = 1.0f;
    float scale_ = 1.0f;
    POINT pos_{};
    Kind kind_ = Kind::Saved;
    std::wstring title_;
    std::wstring subtitle_;
    ULONGLONG start_ms_ = 0;
    bool visible_ = false;
};

}  // namespace ir::gui
