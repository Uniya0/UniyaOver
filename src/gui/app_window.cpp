#include "gui/app_window.h"

#include "app_resources.h"
#include "gui/clip_thumbnail.h"
#include "gui/gdi_utils.h"
#include "gui/region_selector.h"
#include "gui/theme.h"
#include "input/hotkey.h"
#include "util/capability_probe.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/settings.h"

#include <ShlObj.h>
#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace ir::gui {
namespace {

using ir::i18n::tr;

constexpr UINT_PTR kTimerStatus = 1;
constexpr UINT_PTR kTimerAnim = 2;
constexpr UINT kAnimFrameMs = 16;
// Dark text on light (accent) surfaces.
constexpr uint32_t kInk = 0xFF0B0B0B;
constexpr UINT kWmTrayIcon = WM_USER + 200;
constexpr UINT kWmThumbReady = WM_USER + 201;
// wParam: bit0 = ok, bit1 = region. lParam: clip length in 0.1 s.
constexpr UINT kWmClipSaved = WM_USER + 202;
constexpr WPARAM kClipSavedOk = 1;
constexpr WPARAM kClipSavedRegion = 2;
constexpr UINT kTrayId = 1;
// 16:9 thumbnail (2x for crisp draw into 86x48 UI slot).
constexpr int kThumbW = 172;
constexpr int kThumbH = 96;
constexpr float kThumbDrawW = 86.0f;
constexpr float kThumbDrawH = 48.0f;
constexpr float kThumbTextX = 10.0f + kThumbDrawW + 12.0f;

constexpr UINT kTrayOpen = 1001;
constexpr UINT kTrayArm = 1002;
constexpr UINT kTraySave = 1003;
constexpr UINT kTrayRegion = 1004;
constexpr UINT kTrayStop = 1005;
constexpr UINT kTrayExit = 1006;

RECT to_rect(const Gdiplus::RectF& r) {
    RECT out{};
    out.left = static_cast<LONG>(r.X);
    out.top = static_cast<LONG>(r.Y);
    out.right = static_cast<LONG>(r.X + r.Width);
    out.bottom = static_cast<LONG>(r.Y + r.Height);
    return out;
}

int bitrate_mbps(const ir::settings::AppSettings& s) {
    const int b = s.video_bitrate > 0 ? s.video_bitrate : ir::settings::auto_bitrate(s.fps_cap);
    return (std::max)(1, b / 1'000'000);
}

std::wstring utf8(const std::string& s) {
    return utf8_to_wide(s);
}

bool same_binding(const ir::settings::HotkeyBinding& a, const ir::settings::HotkeyBinding& b) {
    return a.vk == b.vk && a.modifiers == b.modifiers;
}

float clamp01f(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float ease_out(float t) {
    const float k = 1.0f - clamp01f(t);
    return 1.0f - k * k * k;
}

uint32_t anim_key(const char* id, int index = 0) {
    uint32_t h = 2166136261u;
    for (const char* p = id; *p; ++p) {
        h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    }
    return (h ^ static_cast<uint32_t>(index)) * 16777619u;
}

uint32_t rect_key(const Gdiplus::RectF& r) {
    uint32_t h = 2166136261u;
    for (float v : {r.X, r.Y, r.Width, r.Height}) {
        h = (h ^ static_cast<uint32_t>(std::lround(v))) * 16777619u;
    }
    return h;
}

float toast_size_factor(int size) {
    constexpr float kFactors[] = {0.8f, 1.0f, 1.3f};
    return kFactors[(std::max)(0, (std::min)(2, size))];
}

std::wstring clip_length_label(double seconds) {
    const int total = (std::max)(1, static_cast<int>(std::lround(seconds)));
    if (total < 60) {
        return std::to_wstring(total) + tr(L" с", L" s");
    }
    const int sec = total % 60;
    return std::to_wstring(total / 60) + (sec < 10 ? L":0" : L":") + std::to_wstring(sec);
}

}  // namespace

AppWindow::~AppWindow() {
    stop_thumbnail_worker();
    save_toast_.destroy();
    tray_remove();
    if (hwnd_) {
        ir::hotkey::uninstall(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(thumb_mutex_);
        thumb_cache_.clear();
    }
    if (gdiplus_token_) {
        release_gdi_cache();
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
    }
}

bool AppWindow::create(
    HINSTANCE instance,
    ir::capture::CaptureSession* capture,
    ir::settings::AppSettings settings,
    ir::hw::HardwareProfile hardware) {
    instance_ = instance;
    capture_ = capture;
    settings_ = settings;
    hw_ = std::move(hardware);

    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        ir::log::error("GdiplusStartup failed");
        return false;
    }

    const wchar_t* class_name = L"UniyaOverMainWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = static_wnd_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = class_name;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIcon) {
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            ir::log::error("RegisterClassExW failed, error=" + std::to_string(err));
            return false;
        }
    }

    hwnd_ = CreateWindowExW(
        0,
        class_name,
        L"UniyaOver",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        width_,
        height_,
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        ir::log::error("CreateWindowExW failed, error=" + std::to_string(GetLastError()));
        return false;
    }

    const ir::hotkey::Callbacks keys{
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
            flash_hotkey(0);
            if (capture_) {
                capture_->arm();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        },
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
            flash_hotkey(1);
            request_full_save();
            InvalidateRect(hwnd_, nullptr, FALSE);
        },
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
            flash_hotkey(3);
            if (capture_) {
                capture_->stop();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        },
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
            flash_hotkey(2);
            begin_region_save();
        },
    };

    if (!ir::hotkey::install(
            hwnd_,
            keys,
            {settings_.hk_arm, settings_.hk_save, settings_.hk_stop, settings_.hk_region})) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    save_toast_.create(instance_);
    apply_toast_style();
    if (capture_) {
        capture_->set_on_clip_saved([hwnd = hwnd_](const ir::capture::ClipSavedEvent& e) {
            const WPARAM flags = (e.ok ? kClipSavedOk : 0) | (e.region ? kClipSavedRegion : 0);
            PostMessageW(hwnd, kWmClipSaved, flags, static_cast<LPARAM>(std::lround(e.seconds * 10.0)));
        });
    }

    sync_clips_path();
    refresh_device_lists();
    start_thumbnail_worker();
    refresh_clips();
    tray_add();
    SetTimer(hwnd_, kTimerStatus, 500, nullptr);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    if (!settings_.onboarding_done) {
        start_onboarding();
    }
    return true;
}

void AppWindow::start_thumbnail_worker() {
    if (thumb_thread_.joinable()) {
        return;
    }
    thumb_stop_.store(false);
    thumb_thread_ = std::thread([this] { thumbnail_worker_main(); });
}

void AppWindow::stop_thumbnail_worker() {
    thumb_stop_.store(true);
    thumb_cv_.notify_all();
    if (thumb_thread_.joinable()) {
        thumb_thread_.join();
    }
    std::lock_guard<std::mutex> lock(thumb_mutex_);
    while (!thumb_queue_.empty()) {
        thumb_queue_.pop();
    }
    thumb_inflight_.clear();
}

void AppWindow::thumbnail_worker_main() {
    for (;;) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(thumb_mutex_);
            thumb_cv_.wait(lock, [this] {
                return thumb_stop_.load() || !thumb_queue_.empty();
            });
            if (thumb_stop_.load() && thumb_queue_.empty()) {
                return;
            }
            path = std::move(thumb_queue_.front());
            thumb_queue_.pop();
        }

        ThumbnailPixels pixels;
        const bool ok = decode_clip_thumbnail_bgra(path, kThumbW, kThumbH, pixels);
        {
            std::lock_guard<std::mutex> lock(thumb_mutex_);
            thumb_inflight_.erase(path);
            if (ok) {
                thumb_ready_pixels_[path] = std::move(pixels);
                thumb_failed_.erase(path);
            } else {
                thumb_failed_.insert(path);
            }
        }
        if (hwnd_ && !thumb_stop_.load()) {
            PostMessageW(hwnd_, kWmThumbReady, 0, 0);
        }
    }
}

void AppWindow::ingest_ready_thumbnails() {
    std::unordered_map<std::wstring, ThumbnailPixels> ready;
    {
        std::lock_guard<std::mutex> lock(thumb_mutex_);
        ready.swap(thumb_ready_pixels_);
    }
    for (auto& [path, pixels] : ready) {
        save_thumbnail_jpeg(pixels, path, kThumbW, kThumbH);
        Gdiplus::Bitmap* bmp = bitmap_from_bgra(pixels);
        if (!bmp) {
            std::lock_guard<std::mutex> lock(thumb_mutex_);
            thumb_failed_.insert(path);
            continue;
        }
        std::lock_guard<std::mutex> lock(thumb_mutex_);
        thumb_cache_[path].reset(bmp);
        thumb_failed_.erase(path);
    }
}

void AppWindow::request_thumbnails_for_clips() {
    std::unordered_set<std::wstring> keep;
    keep.reserve(clips_.size());
    for (const auto& c : clips_) {
        keep.insert(c.path);
    }

    {
        std::lock_guard<std::mutex> lock(thumb_mutex_);
        for (auto it = thumb_cache_.begin(); it != thumb_cache_.end();) {
            if (keep.count(it->first) == 0) {
                it = thumb_cache_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = thumb_failed_.begin(); it != thumb_failed_.end();) {
            if (keep.count(*it) == 0) {
                it = thumb_failed_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = thumb_ready_pixels_.begin(); it != thumb_ready_pixels_.end();) {
            if (keep.count(it->first) == 0) {
                it = thumb_ready_pixels_.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& c : clips_) {
            if (thumb_cache_.count(c.path) || thumb_inflight_.count(c.path) ||
                thumb_failed_.count(c.path) || thumb_ready_pixels_.count(c.path)) {
                continue;
            }
            // Fast path: JPEG already on disk — load on UI thread, no decode.
            if (thumbnail_cache_valid(c.path, kThumbW, kThumbH)) {
                continue;
            }
            thumb_inflight_.insert(c.path);
            thumb_queue_.push(c.path);
        }
        if (!thumb_queue_.empty()) {
            thumb_cv_.notify_all();
        }
    }

    // Load disk-cached JPEGs on the UI thread (GDI+).
    for (const auto& c : clips_) {
        {
            std::lock_guard<std::mutex> lock(thumb_mutex_);
            if (thumb_cache_.count(c.path)) {
                continue;
            }
        }
        if (!thumbnail_cache_valid(c.path, kThumbW, kThumbH)) {
            continue;
        }
        Gdiplus::Bitmap* bmp = load_thumbnail_bitmap_from_cache(c.path, kThumbW, kThumbH);
        if (!bmp) {
            // Corrupt cache — re-decode.
            std::lock_guard<std::mutex> lock(thumb_mutex_);
            if (!thumb_inflight_.count(c.path) && !thumb_failed_.count(c.path)) {
                thumb_inflight_.insert(c.path);
                thumb_queue_.push(c.path);
                thumb_cv_.notify_all();
            }
            continue;
        }
        std::lock_guard<std::mutex> lock(thumb_mutex_);
        thumb_cache_[c.path].reset(bmp);
    }
}

Gdiplus::Bitmap* AppWindow::thumbnail_for(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(thumb_mutex_);
    const auto it = thumb_cache_.find(path);
    if (it == thumb_cache_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void AppWindow::rebind_hotkeys() {
    ir::hotkey::reinstall(
        {settings_.hk_arm, settings_.hk_save, settings_.hk_stop, settings_.hk_region});
}

void AppWindow::refresh_device_lists() {
    monitors_ = ir::devices::list_monitors();
    audio_devices_ = ir::devices::list_playback_devices();
}

void AppWindow::sync_clips_path() {
    ir::paths::set_clips_directory_override(settings_.clips_folder);
}

void AppWindow::apply_and_save() {
    ir::hw::clamp_settings(settings_, hw_);
    if (settings_.video_bitrate <= 0) {
        settings_.video_bitrate = ir::settings::auto_bitrate(settings_.fps_cap);
    }
    sync_clips_path();
    if (capture_) {
        capture_->set_settings(settings_);
    }
    ir::settings::save(settings_);
    ir::log::info("Settings: " + ir::settings::describe(settings_));
}

bool AppWindow::browse_clips_folder() {
    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return false;
    }
    DWORD opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(tr(L"Папка для клипов UniyaOver", L"UniyaOver clips folder"));
    hr = dialog->Show(hwnd_);
    if (FAILED(hr)) {
        dialog->Release();
        return false;
    }
    IShellItem* item = nullptr;
    hr = dialog->GetResult(&item);
    dialog->Release();
    if (FAILED(hr) || !item) {
        return false;
    }
    PWSTR path = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    if (FAILED(hr) || !path) {
        return false;
    }
    settings_.clips_folder = wide_to_utf8(path);
    CoTaskMemFree(path);
    return true;
}

int AppWindow::run_message_loop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK AppWindow::static_wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<AppWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) {
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }
    return self->wnd_proc(msg, w_param, l_param);
}

void AppWindow::tray_add() {
    ZeroMemory(&tray_, sizeof(tray_));
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = kTrayId;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = kWmTrayIcon;
    tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    if (!tray_.hIcon) {
        tray_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    wcsncpy_s(tray_.szTip, L"UniyaOver", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &tray_);
}

void AppWindow::tray_remove() {
    if (tray_.cbSize) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        ZeroMemory(&tray_, sizeof(tray_));
    }
}

void AppWindow::minimize_to_tray() {
    ShowWindow(hwnd_, SW_HIDE);
    in_tray_ = true;
}

void AppWindow::restore_from_tray() {
    ShowWindow(hwnd_, SW_SHOW);
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    in_tray_ = false;
}

void AppWindow::tray_show_menu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayOpen, tr(L"Открыть UniyaOver", L"Open UniyaOver"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayArm, tr(L"Старт буфера", L"Start buffer"));
    AppendMenuW(menu, MF_STRING, kTraySave, tr(L"Сохранить весь экран", L"Save full screen"));
    AppendMenuW(menu, MF_STRING, kTrayRegion, tr(L"Сохранить область", L"Save region"));
    AppendMenuW(menu, MF_STRING, kTrayStop, tr(L"Стоп буфера", L"Stop buffer"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, tr(L"Выход", L"Exit"));
    SetForegroundWindow(hwnd_);
    const int cmd = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case kTrayOpen:
            restore_from_tray();
            break;
        case kTrayArm:
            if (capture_) {
                capture_->arm();
            }
            break;
        case kTraySave:
            request_full_save();
            break;
        case kTrayRegion:
            begin_region_save();
            break;
        case kTrayStop:
            if (capture_) {
                capture_->stop();
            }
            break;
        case kTrayExit:
            DestroyWindow(hwnd_);
            break;
        default:
            break;
    }
}

LRESULT AppWindow::wnd_proc(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (listen_ == ListenTarget::None &&
        ir::hotkey::handle_message(hwnd_, msg, w_param, l_param)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    switch (msg) {
        case WM_SIZE:
            if (w_param == SIZE_MINIMIZED) {
                minimize_to_tray();
                return 0;
            }
            width_ = LOWORD(l_param);
            height_ = HIWORD(l_param);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_SYSCOMMAND:
            if ((w_param & 0xFFF0) == SC_MINIMIZE) {
                minimize_to_tray();
                return 0;
            }
            break;
        case kWmTrayIcon:
            if (l_param == WM_LBUTTONDBLCLK || l_param == WM_LBUTTONUP) {
                restore_from_tray();
            } else if (l_param == WM_RBUTTONUP) {
                tray_show_menu();
            }
            return 0;
        case kWmThumbReady:
            ingest_ready_thumbnails();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case kWmClipSaved:
            on_clip_saved(w_param, l_param);
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
            return 0;
        case WM_MOUSELEAVE:
            mouse_tracking_ = false;
            on_mouse_move(-10000, -10000);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l_param) == HTCLIENT && hot_index_ >= 0) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_ACTIVATE:
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case WM_TIMER:
            if (w_param == kTimerAnim) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (w_param == kTimerStatus) {
                // Pick up newly finished saves without spamming the disk every tick.
                if (++clip_refresh_ticks_ >= 4) {
                    clip_refresh_ticks_ = 0;
                    refresh_clips();
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_LBUTTONUP:
            on_click(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
            return 0;
        case WM_RBUTTONUP:
            on_rclick(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (listen_ != ListenTarget::None) {
                // Ignore key auto-repeat.
                if ((l_param & (1 << 30)) == 0) {
                    on_listen_keydown(w_param);
                }
                return 0;
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (listen_ != ListenTarget::None) {
                on_listen_keyup(w_param);
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerStatus);
            KillTimer(hwnd_, kTimerAnim);
            stop_thumbnail_worker();
            save_toast_.destroy();
            tray_remove();
            ir::hotkey::uninstall(hwnd_);
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, w_param, l_param);
}

void AppWindow::refresh_clips() {
    clips_ = scan_clips(40);
    const std::wstring newest = clips_.empty() ? std::wstring() : clips_.front().path;
    if (clips_scanned_ && !newest.empty() && newest != newest_clip_) {
        new_clip_ms_ = GetTickCount64();
    }
    newest_clip_ = newest;
    clips_scanned_ = true;
    free_space_ = free_space_display(clips_dir_wide());
    request_thumbnails_for_clips();
}

void AppWindow::show_clip_context_menu(int index, int x, int y) {
    if (index < 0 || index >= static_cast<int>(clips_.size())) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, tr(L"Открыть", L"Open"));
    AppendMenuW(menu, MF_STRING, 2, tr(L"Показать в проводнике", L"Show in Explorer"));
    POINT pt{x, y};
    ClientToScreen(hwnd_, &pt);
    const int cmd =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) {
        open_clip(clips_[static_cast<size_t>(index)].path);
    } else if (cmd == 2) {
        reveal_clip(clips_[static_cast<size_t>(index)].path);
    }
}

void AppWindow::begin_hotkey_listen(ListenTarget target) {
    listen_ = target;
    listen_mods_ = 0;
    ir::hotkey::suspend();
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::cancel_hotkey_listen() {
    if (listen_ == ListenTarget::None) {
        return;
    }
    listen_ = ListenTarget::None;
    listen_mods_ = 0;
    ir::hotkey::reinstall(
        {settings_.hk_arm, settings_.hk_save, settings_.hk_stop, settings_.hk_region});
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::on_listen_keyup(WPARAM vk) {
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        listen_mods_ &= ~static_cast<unsigned>(MOD_CONTROL);
    } else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        listen_mods_ &= ~static_cast<unsigned>(MOD_SHIFT);
    } else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        listen_mods_ &= ~static_cast<unsigned>(MOD_ALT);
    } else if (vk == VK_LWIN || vk == VK_RWIN) {
        listen_mods_ &= ~static_cast<unsigned>(MOD_WIN);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::on_listen_keydown(WPARAM vk) {
    if (vk == VK_ESCAPE) {
        cancel_hotkey_listen();
        return;
    }

    // Build modifiers only from keys pressed during this listen session.
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        listen_mods_ |= MOD_CONTROL;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        listen_mods_ |= MOD_SHIFT;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        listen_mods_ |= MOD_ALT;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (vk == VK_LWIN || vk == VK_RWIN) {
        listen_mods_ |= MOD_WIN;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    commit_hotkey(static_cast<unsigned>(vk), listen_mods_);
}

bool AppWindow::commit_hotkey(unsigned vk, unsigned modifiers) {
    const bool is_fn = (vk >= VK_F1 && vk <= VK_F24);
    const unsigned mods =
        modifiers & (MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN);
    const int mod_count =
        ((mods & MOD_CONTROL) ? 1 : 0) + ((mods & MOD_SHIFT) ? 1 : 0) +
        ((mods & MOD_ALT) ? 1 : 0) + ((mods & MOD_WIN) ? 1 : 0);

    // Ctrl+X (1 mod) or Ctrl+Shift+X (2 mods) or F9 alone — OK.
    if (!is_fn && mod_count < 1) {
        ir::log::warn("Нужно: Ctrl/Alt/Shift/Win + клавиша (например Ctrl+X), либо F1–F12");
        return false;
    }

    ir::settings::HotkeyBinding b;
    b.modifiers = mods;
    b.vk = vk;

    auto conflicts = [&](const ir::settings::HotkeyBinding& other) {
        return same_binding(b, other);
    };

    if (listen_ == ListenTarget::Arm) {
        if (conflicts(settings_.hk_save) || conflicts(settings_.hk_stop) ||
            conflicts(settings_.hk_region)) {
            ir::log::warn("Hotkey conflicts with another binding");
            return false;
        }
        settings_.hk_arm = b;
    } else if (listen_ == ListenTarget::Save) {
        if (conflicts(settings_.hk_arm) || conflicts(settings_.hk_stop) ||
            conflicts(settings_.hk_region)) {
            ir::log::warn("Hotkey conflicts with another binding");
            return false;
        }
        settings_.hk_save = b;
    } else if (listen_ == ListenTarget::Stop) {
        if (conflicts(settings_.hk_arm) || conflicts(settings_.hk_save) ||
            conflicts(settings_.hk_region)) {
            ir::log::warn("Hotkey conflicts with another binding");
            return false;
        }
        settings_.hk_stop = b;
    } else if (listen_ == ListenTarget::Region) {
        if (conflicts(settings_.hk_arm) || conflicts(settings_.hk_save) ||
            conflicts(settings_.hk_stop)) {
            ir::log::warn("Hotkey conflicts with another binding");
            return false;
        }
        settings_.hk_region = b;
    } else {
        return false;
    }

    listen_ = ListenTarget::None;
    listen_mods_ = 0;

    if (!ir::hotkey::reinstall(
            {settings_.hk_arm, settings_.hk_save, settings_.hk_stop, settings_.hk_region})) {
        ir::log::error(
            "RegisterHotKey failed for " + ir::settings::hotkey_label(b) +
            " (занято системой/другой программой?)");
        if (!ir::hotkey::reinstall(
                {settings_.hk_arm, settings_.hk_save, settings_.hk_stop, settings_.hk_region})) {
            settings_.hk_arm = {MOD_CONTROL | MOD_SHIFT, 'R'};
            settings_.hk_save = {MOD_CONTROL | MOD_SHIFT, 'X'};
            settings_.hk_stop = {MOD_CONTROL | MOD_SHIFT, 'S'};
            settings_.hk_region = {MOD_CONTROL | MOD_SHIFT, 'A'};
            ir::hotkey::reinstall(
                {settings_.hk_arm, settings_.hk_save, settings_.hk_stop, settings_.hk_region});
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }

    apply_and_save();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ir::log::info("Hotkey set: " + ir::settings::hotkey_label(b));
    return true;
}

void AppWindow::request_full_save() {
    if (!capture_) {
        return;
    }
    if (!capture_->is_armed()) {
        show_idle_toast();
        return;
    }
    capture_->request_save();
    refresh_clips();
}

void AppWindow::apply_toast_style() {
    save_toast_.set_style(
        static_cast<SaveToast::Position>(settings_.toast_position),
        toast_size_factor(settings_.toast_size));
}

void AppWindow::show_toast_preview() {
    if (!settings_.toast_enabled) {
        return;
    }
    save_toast_.show(
        SaveToast::Kind::Saved,
        tr(L"Сохранено", L"Saved"),
        tr(L"так будет выглядеть уведомление", L"this is how the notification looks"),
        MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
}

void AppWindow::show_idle_toast() {
    if (!settings_.toast_enabled) {
        return;
    }
    save_toast_.show(
        SaveToast::Kind::Warning,
        tr(L"Буфер не запущен", L"Buffer is off"),
        utf8(ir::settings::hotkey_label(settings_.hk_arm)) + tr(L" — старт", L" — start"),
        RECT{});
}

void AppWindow::on_clip_saved(WPARAM flags, LPARAM deciseconds) {
    refresh_clips();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!settings_.toast_enabled) {
        return;
    }

    const RECT monitor = capture_ ? capture_->capture_desktop_rect() : RECT{};
    const bool region = (flags & kClipSavedRegion) != 0;
    if ((flags & kClipSavedOk) != 0) {
        const std::wstring what =
            region ? tr(L"область", L"region") : tr(L"весь экран", L"full screen");
        save_toast_.show(
            SaveToast::Kind::Saved,
            tr(L"Сохранено", L"Saved"),
            what + L" · " + clip_length_label(static_cast<double>(deciseconds) / 10.0),
            monitor);
    } else if (!region && deciseconds == 0) {
        save_toast_.show(
            SaveToast::Kind::Warning,
            tr(L"Буфер ещё пуст", L"Buffer is empty"),
            tr(L"подождите пару секунд", L"wait a couple of seconds"),
            monitor);
    } else {
        save_toast_.show(
            SaveToast::Kind::Error,
            tr(L"Не удалось сохранить", L"Couldn't save"),
            tr(L"подробности в логе", L"see the log for details"),
            monitor);
    }
}

void AppWindow::begin_region_save() {
    if (!capture_ || !capture_->is_armed()) {
        ir::log::warn("Region save: сначала запустите буфер");
        show_idle_toast();
        return;
    }
    const int cw = capture_->capture_width();
    const int ch = capture_->capture_height();
    RECT mon = capture_->capture_desktop_rect();
    if (cw < 32 || ch < 32 || mon.right <= mon.left) {
        ir::log::error("Region save: нет активного захвата");
        return;
    }

    // Freeze ring first so selection UI never lands in the clip.
    if (!capture_->freeze_region_buffer()) {
        return;
    }

    ir::hotkey::suspend();
    RECT crop{};
    const bool ok = pick_screen_region(hwnd_, mon, cw, ch, &crop);
    rebind_hotkeys();
    if (!ok) {
        capture_->cancel_region_freeze();
        return;
    }
    capture_->complete_region_save(crop);
    refresh_clips();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::on_click(int x, int y) {
    if (tour_step_ != TourStep::None) {
        handle_onboarding_click(x, y);
        return;
    }

    if (point_in_rect(x, y, tour_help_hit_)) {
        start_onboarding();
        return;
    }
    if (point_in_rect(x, y, lang_ru_hit_)) {
        set_language(ir::i18n::Lang::Ru);
        return;
    }
    if (point_in_rect(x, y, lang_en_hit_)) {
        set_language(ir::i18n::Lang::En);
        return;
    }
    for (const RECT& r : home_hk_hit_) {
        if (point_in_rect(x, y, r)) {
            cancel_hotkey_listen();
            page_ = Page::Settings;
            settings_tab_ = SettingsTab::Hotkeys;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    if (point_in_rect(x, y, nav_home_)) {
        page_ = Page::Home;
        cancel_hotkey_listen();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (point_in_rect(x, y, nav_lib_)) {
        page_ = Page::Library;
        cancel_hotkey_listen();
        refresh_clips();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (point_in_rect(x, y, nav_set_)) {
        page_ = Page::Settings;
        cancel_hotkey_listen();
        refresh_device_lists();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (point_in_rect(x, y, telegram_hit_)) {
        ShellExecuteW(nullptr, L"open", L"https://t.me/UNIYA0", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (point_in_rect(x, y, all_clips_hit_)) {
        page_ = Page::Library;
        refresh_clips();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (point_in_rect(x, y, path_hit_)) {
        open_clips_folder();
        return;
    }

    for (const auto& hit : clip_hits_) {
        if (point_in_rect(x, y, hit.menu) || point_in_rect(x, y, hit.row)) {
            if (point_in_rect(x, y, hit.menu)) {
                show_clip_context_menu(hit.index, x, y);
            } else {
                open_clip(clips_[static_cast<size_t>(hit.index)].path);
            }
            return;
        }
    }

    if (page_ != Page::Settings) {
        return;
    }

    if (point_in_rect(x, y, tab_quality_)) {
        if (settings_tab_ != SettingsTab::Quality) {
            cancel_hotkey_listen();
            settings_tab_ = SettingsTab::Quality;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (point_in_rect(x, y, tab_hotkeys_)) {
        if (settings_tab_ != SettingsTab::Hotkeys) {
            settings_tab_ = SettingsTab::Hotkeys;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (point_in_rect(x, y, tab_devices_)) {
        if (settings_tab_ != SettingsTab::Devices) {
            cancel_hotkey_listen();
            settings_tab_ = SettingsTab::Devices;
            refresh_device_lists();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (point_in_rect(x, y, tab_clips_)) {
        if (settings_tab_ != SettingsTab::Clips) {
            cancel_hotkey_listen();
            settings_tab_ = SettingsTab::Clips;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (point_in_rect(x, y, tab_notify_)) {
        if (settings_tab_ != SettingsTab::Notify) {
            cancel_hotkey_listen();
            settings_tab_ = SettingsTab::Notify;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (point_in_rect(x, y, toast_preview_hit_)) {
        show_toast_preview();
        return;
    }
    bool toast_changed = false;
    if (point_in_rect(x, y, toast_on_hit_) && !settings_.toast_enabled) {
        settings_.toast_enabled = true;
        toast_changed = true;
    } else if (point_in_rect(x, y, toast_off_hit_) && settings_.toast_enabled) {
        settings_.toast_enabled = false;
        toast_changed = true;
    }
    for (int i = 0; i < 3 && !toast_changed; ++i) {
        if (point_in_rect(x, y, toast_size_hit_[i])) {
            settings_.toast_size = i;
            toast_changed = true;
        }
    }
    for (int i = 0; i < 6 && !toast_changed; ++i) {
        if (point_in_rect(x, y, toast_pos_hit_[i])) {
            settings_.toast_position = i;
            toast_changed = true;
        }
    }
    if (toast_changed) {
        apply_toast_style();
        apply_and_save();
        InvalidateRect(hwnd_, nullptr, FALSE);
        show_toast_preview();
        return;
    }

    bool changed = false;
    if (point_in_rect(x, y, codec_auto_)) {
        settings_.video_codec = "auto";
        changed = true;
    } else if (point_in_rect(x, y, codec_h264_)) {
        settings_.video_codec = "h264";
        changed = true;
    } else if (point_in_rect(x, y, codec_hevc_) && ir::hw::hevc_available(hw_)) {
        settings_.video_codec = "hevc";
        changed = true;
    } else if (point_in_rect(x, y, fps60_) && ir::hw::fps_allowed(hw_, 60)) {
        settings_.fps_cap = 60;
        settings_.video_bitrate = ir::settings::auto_bitrate(60);
        changed = true;
    } else if (point_in_rect(x, y, fps144_) && ir::hw::fps_allowed(hw_, 144)) {
        settings_.fps_cap = 144;
        settings_.video_bitrate = ir::settings::auto_bitrate(144);
        changed = true;
    } else if (point_in_rect(x, y, fps240_) && ir::hw::fps_allowed(hw_, 240)) {
        settings_.fps_cap = 240;
        settings_.video_bitrate = ir::settings::auto_bitrate(240);
        changed = true;
    } else if (point_in_rect(x, y, bitrate_minus_)) {
        int mbps = bitrate_mbps(settings_);
        mbps = (std::max)(5, mbps - 5);
        settings_.video_bitrate = mbps * 1'000'000;
        changed = true;
    } else if (point_in_rect(x, y, bitrate_plus_)) {
        int mbps = bitrate_mbps(settings_);
        mbps = (std::min)(hw_.max_bitrate_mbps, mbps + 5);
        settings_.video_bitrate = mbps * 1'000'000;
        changed = true;
    } else if (point_in_rect(x, y, buffer_minus_)) {
        settings_.buffer_sec = (std::max)(5, settings_.buffer_sec - 5);
        changed = true;
    } else if (point_in_rect(x, y, buffer_plus_)) {
        settings_.buffer_sec = (std::min)(hw_.max_buffer_sec, settings_.buffer_sec + 5);
        changed = true;
    } else if (point_in_rect(x, y, hk_arm_hit_)) {
        begin_hotkey_listen(ListenTarget::Arm);
        return;
    } else if (point_in_rect(x, y, hk_save_hit_)) {
        begin_hotkey_listen(ListenTarget::Save);
        return;
    } else if (point_in_rect(x, y, hk_stop_hit_)) {
        begin_hotkey_listen(ListenTarget::Stop);
        return;
    } else if (point_in_rect(x, y, hk_region_hit_)) {
        begin_hotkey_listen(ListenTarget::Region);
        return;
    } else if (point_in_rect(x, y, monitor_hit_)) {
        if (monitors_.empty()) {
            refresh_device_lists();
        }
        if (monitors_.empty()) {
            settings_.monitor_index = -1;
        } else if (settings_.monitor_index < 0) {
            settings_.monitor_index = 0;
        } else {
            settings_.monitor_index += 1;
            if (settings_.monitor_index >= static_cast<int>(monitors_.size())) {
                settings_.monitor_index = -1;
            }
        }
        changed = true;
    } else if (point_in_rect(x, y, audio_hit_)) {
        if (audio_devices_.empty()) {
            refresh_device_lists();
        }
        if (audio_devices_.empty()) {
            settings_.audio_device_id.clear();
        } else if (settings_.audio_device_id.empty()) {
            settings_.audio_device_id = audio_devices_.front().id;
        } else {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(audio_devices_.size()); ++i) {
                if (audio_devices_[static_cast<size_t>(i)].id == settings_.audio_device_id) {
                    idx = i;
                    break;
                }
            }
            if (idx + 1 >= static_cast<int>(audio_devices_.size())) {
                settings_.audio_device_id.clear();
            } else {
                settings_.audio_device_id =
                    audio_devices_[static_cast<size_t>(idx + 1)].id;
            }
        }
        changed = true;
    } else if (point_in_rect(x, y, clips_browse_hit_)) {
        if (browse_clips_folder()) {
            changed = true;
            refresh_clips();
        }
    } else if (point_in_rect(x, y, clips_reset_hit_)) {
        settings_.clips_folder.clear();
        changed = true;
        refresh_clips();
    }

    if (changed) {
        apply_and_save();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::on_rclick(int x, int y) {
    for (const auto& hit : clip_hits_) {
        if (point_in_rect(x, y, hit.row)) {
            show_clip_context_menu(hit.index, x, y);
            return;
        }
    }
}

float AppWindow::smooth(uint32_t key, float target, float speed, float eps) {
    auto [it, inserted] = anim_.try_emplace(key, AnimValue{target, frame_no_});
    AnimValue& a = it->second;
    a.seen = frame_no_;
    if (inserted) {
        return target;
    }
    const float diff = target - a.value;
    if (std::fabs(diff) <= eps) {
        a.value = target;
        return target;
    }
    a.value += diff * (1.0f - std::exp(-speed * frame_dt_));
    anim_busy_ = true;
    return a.value;
}

float AppWindow::hover_amount(const Gdiplus::RectF& r) {
    if (tour_step_ != TourStep::None && !painting_tour_) {
        return smooth(rect_key(r), 0.0f, 18.0f);
    }
    hot_rects_.push_back(to_rect(r));
    return smooth(rect_key(r), point_in_rectf(mouse_.x, mouse_.y, r) ? 1.0f : 0.0f, 18.0f);
}

float AppWindow::page_progress(float delay_ms, float duration_ms) {
    const float t = (static_cast<float>(now_ms_ - page_enter_ms_) - delay_ms) / duration_ms;
    if (t < 1.0f) {
        anim_busy_ = true;
    }
    return ease_out(t);
}

void AppWindow::flash_hotkey(int index) {
    if (index >= 0 && index < 4) {
        flash_ms_[index] = GetTickCount64();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::on_mouse_move(int x, int y) {
    if (!mouse_tracking_ && x > -10000) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        mouse_tracking_ = TrackMouseEvent(&tme) != FALSE;
    }
    mouse_ = POINT{x, y};
    int hot = -1;
    for (int i = 0; i < static_cast<int>(hot_rects_.size()); ++i) {
        if (point_in_rect(x, y, hot_rects_[static_cast<size_t>(i)])) {
            hot = i;
            break;
        }
    }
    if (hot != hot_index_) {
        hot_index_ = hot;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::set_language(ir::i18n::Lang lang) {
    if (ir::i18n::language() == lang && !settings_.language.empty()) {
        return;
    }
    ir::i18n::set_language(lang);
    settings_.language = ir::i18n::to_code(lang);
    refresh_device_lists();
    refresh_clips();
    apply_and_save();
    page_enter_ms_ = GetTickCount64();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::update_anim_timer() {
    const bool visible = hwnd_ && !in_tray_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    const bool focused = GetForegroundWindow() == hwnd_;
    const bool armed = capture_ && capture_->is_armed();
    // Ambient motion (buffer shimmer, pulsing status) only while the user is looking at
    // the window; transitions always finish.
    const bool ambient = focused && (armed || listen_ != ListenTarget::None);
    const bool want = visible && (anim_busy_ || ambient);
    if (want && !anim_timer_on_) {
        SetTimer(hwnd_, kTimerAnim, kAnimFrameMs, nullptr);
        anim_timer_on_ = true;
    } else if (!want && anim_timer_on_) {
        KillTimer(hwnd_, kTimerAnim);
        anim_timer_on_ = false;
    }
}

void AppWindow::on_paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = rc.right - rc.left;
    height_ = rc.bottom - rc.top;

    now_ms_ = GetTickCount64();
    frame_dt_ = frame_ms_ ? (std::min)(0.05f, static_cast<float>(now_ms_ - frame_ms_) / 1000.0f) : 0.0f;
    frame_ms_ = now_ms_;
    ++frame_no_;
    anim_busy_ = false;
    hot_rects_.clear();
    if (page_enter_ms_ == 0 || page_ != painted_page_ || settings_tab_ != painted_tab_) {
        page_enter_ms_ = now_ms_;
        painted_page_ = page_;
        painted_tab_ = settings_tab_;
    }
    if (tour_step_ != painted_tour_) {
        tour_enter_ms_ = now_ms_;
        painted_tour_ = tour_step_;
    }
    const bool armed = capture_ && capture_->is_armed();
    if (armed && armed_since_ms_ == 0) {
        armed_since_ms_ = now_ms_;
    } else if (!armed) {
        armed_since_ms_ = 0;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width_, height_);
    HGDIOBJ old = SelectObject(mem, bmp);

    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        Gdiplus::SolidBrush bg(argb(theme::kBg));
        g.FillRectangle(&bg, 0, 0, width_, height_);

        clip_hits_.clear();
        path_hit_ = {};
        all_clips_hit_ = {};
        telegram_hit_ = {};
        hk_arm_hit_ = {};
        hk_save_hit_ = {};
        hk_stop_hit_ = {};
        hk_region_hit_ = {};
        monitor_hit_ = {};
        audio_hit_ = {};
        clips_browse_hit_ = {};
        clips_reset_hit_ = {};
        buffer_minus_ = {};
        buffer_plus_ = {};
        bitrate_minus_ = {};
        bitrate_plus_ = {};
        fps60_ = {};
        fps144_ = {};
        fps240_ = {};
        codec_auto_ = {};
        codec_h264_ = {};
        codec_hevc_ = {};
        tab_quality_ = {};
        tab_hotkeys_ = {};
        tab_devices_ = {};
        tab_clips_ = {};
        tab_notify_ = {};
        toast_on_hit_ = {};
        toast_off_hit_ = {};
        for (RECT& r : toast_size_hit_) {
            r = {};
        }
        for (RECT& r : toast_pos_hit_) {
            r = {};
        }
        for (RECT& r : home_hk_hit_) {
            r = {};
        }
        toast_preview_hit_ = {};
        lang_ru_hit_ = {};
        lang_en_hit_ = {};
        tour_next_hit_ = {};
        tour_skip_hit_ = {};
        tour_help_hit_ = {};

        paint_sidebar(g);
        if (page_ == Page::Home) {
            paint_home(g);
        } else if (page_ == Page::Library) {
            paint_library(g);
        } else {
            paint_settings(g);
        }

        // Page / tab entrance: content fades up out of the background.
        const float enter = page_progress(0.0f, 240.0f);
        if (enter < 1.0f) {
            Gdiplus::SolidBrush veil(argb(with_alpha(theme::kBg, (1.0f - enter) * 0.9f)));
            const float left = static_cast<float>(theme::kSidebarW);
            g.FillRectangle(&veil, left, 0.0f, static_cast<float>(width_) - left, static_cast<float>(height_));
        }

        if (tour_step_ != TourStep::None) {
            hot_rects_.clear();
            painting_tour_ = true;
            paint_onboarding(g);
            painting_tour_ = false;
        }
    }

    BitBlt(hdc, 0, 0, width_, height_, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);

    if (anim_.size() > 768) {
        for (auto it = anim_.begin(); it != anim_.end();) {
            it = it->second.seen != frame_no_ ? anim_.erase(it) : std::next(it);
        }
    }
    hot_index_ = -1;
    for (int i = 0; i < static_cast<int>(hot_rects_.size()); ++i) {
        if (point_in_rect(mouse_.x, mouse_.y, hot_rects_[static_cast<size_t>(i)])) {
            hot_index_ = i;
            break;
        }
    }
    update_anim_timer();
}

void AppWindow::paint_sidebar(Gdiplus::Graphics& g) {
    using namespace theme;
    const float sw = static_cast<float>(kSidebarW);
    fill_round_rect(g, Gdiplus::RectF(0, 0, sw, static_cast<float>(height_)), 0, kSidebar);

    draw_brand_u(g, Gdiplus::RectF(18, 20, 32, 32), kText);
    draw_text(g, L"UniyaOver", Gdiplus::RectF(56, 18, 130, 26), kText, 11.0f, true);
    draw_text(g, L"v1.2.0", Gdiplus::RectF(56, 42, 130, 18), kTextMuted, 9.0f, false);

    const float nav_y0 = 90.0f;
    const float nav_step = 48.0f;
    const int active = page_ == Page::Home ? 0 : (page_ == Page::Library ? 1 : 2);
    const wchar_t* labels[] = {
        tr(L"Главная", L"Home"), tr(L"Библиотека", L"Library"), tr(L"Настройки", L"Settings")};
    RECT* hits[] = {&nav_home_, &nav_lib_, &nav_set_};

    float hovers[3] = {};
    for (int i = 0; i < 3; ++i) {
        const Gdiplus::RectF r(12, nav_y0 + nav_step * static_cast<float>(i), sw - 24, static_cast<float>(kNavH));
        hovers[i] = hover_amount(r);
        if (i != active && hovers[i] > 0.01f) {
            fill_round_rect(g, r, 10, with_alpha(kCardHover, hovers[i] * 0.8f));
        }
        *hits[i] = to_rect(r);
    }
    // Active pill glides between items.
    const float pill_y = smooth(anim_key("nav.pill"), nav_y0 + nav_step * static_cast<float>(active), 16.0f, 0.3f);
    const Gdiplus::RectF pill(12, pill_y, sw - 24, static_cast<float>(kNavH));
    fill_round_rect(g, pill, 10, kNavActive);
    fill_round_rect(g, Gdiplus::RectF(pill.X + 7, pill.Y + 12, 3, static_cast<float>(kNavH) - 24), 1.5f, kAccent);

    for (int i = 0; i < 3; ++i) {
        const float on = smooth(anim_key("nav.on", i), i == active ? 1.0f : 0.0f, 14.0f);
        const Gdiplus::RectF r(12, nav_y0 + nav_step * static_cast<float>(i), sw - 24, static_cast<float>(kNavH));
        draw_text(
            g,
            labels[i],
            Gdiplus::RectF(r.X + 18 + 4.0f * on, r.Y, r.Width - 24, r.Height),
            mix_argb(kTextDim, kText, (std::max)(on, hovers[i] * 0.7f)),
            11.0f,
            i == active,
            Gdiplus::StringAlignmentNear,
            Gdiplus::StringAlignmentCenter);
    }

    // Language switch: two segments with a sliding knob.
    {
        const bool en = ir::i18n::language() == ir::i18n::Lang::En;
        const Gdiplus::RectF track(20, static_cast<float>(height_) - 148, 84, 26);
        fill_round_rect(g, track, 13, kCard);
        draw_round_rect(g, track, 13, kBorder, 1.0f);
        const float seg_w = track.Width * 0.5f;
        const float knob_x = smooth(anim_key("lang.knob"), track.X + (en ? seg_w : 0.0f), 18.0f, 0.3f);
        fill_round_rect(g, Gdiplus::RectF(knob_x + 2, track.Y + 2, seg_w - 4, track.Height - 4), 11, kAccent);
        const wchar_t* codes[] = {L"RU", L"EN"};
        RECT* lang_hits[] = {&lang_ru_hit_, &lang_en_hit_};
        for (int i = 0; i < 2; ++i) {
            const Gdiplus::RectF seg(track.X + seg_w * static_cast<float>(i), track.Y, seg_w, track.Height);
            const float h = hover_amount(seg);
            const float cover = clamp01f(1.0f - std::fabs(knob_x - seg.X) / seg_w);
            draw_text(
                g,
                codes[i],
                seg,
                mix_argb(mix_argb(kTextMuted, kText, h), kInk, cover),
                8.5f,
                true,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
            *lang_hits[i] = to_rect(seg);
        }
    }

    const bool armed = capture_ && capture_->is_armed();
    const float live = smooth(anim_key("status.live"), armed ? 1.0f : 0.0f, 8.0f);
    {
        const float sy = static_cast<float>(height_) - 110;
        const float cx = 25.0f;
        const float cy = sy + 10.0f;
        if (armed) {
            // Recording ripple.
            const float phase = static_cast<float>(now_ms_ % 1600) / 1600.0f;
            const float rr = 4.0f + 9.0f * ease_out(phase);
            Gdiplus::Pen ripple(argb(with_alpha(kStatusLive, (1.0f - phase) * 0.55f * live)), 1.2f);
            g.DrawEllipse(&ripple, cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
        }
        Gdiplus::Pen ring(argb(mix_argb(kStatusIdle, kStatusLive, live)), 1.4f);
        g.DrawEllipse(&ring, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        if (live > 0.01f) {
            const float r = 3.0f * live;
            Gdiplus::SolidBrush dot(argb(with_alpha(kStatusLive, live)));
            g.FillEllipse(&dot, cx - r, cy - r, r * 2.0f, r * 2.0f);
        }
        draw_text(
            g,
            armed ? L"BUFFERING" : L"IDLE",
            Gdiplus::RectF(36, sy, sw - 56, 20),
            mix_argb(kStatusIdle, kStatusLive, live),
            9.5f,
            armed);
    }

    Gdiplus::RectF help(20, static_cast<float>(height_) - 88, sw - 40, 18);
    const float help_h = hover_amount(help);
    draw_text(
        g,
        tr(L"Обучение ›", L"Tutorial ›"),
        Gdiplus::RectF(help.X + 3.0f * help_h, help.Y, help.Width, help.Height),
        mix_argb(kTextDim, kText, help_h),
        9.0f,
        false);
    tour_help_hit_ = to_rect(help);
    draw_text(
        g,
        L"© UNIYA",
        Gdiplus::RectF(20, static_cast<float>(height_) - 64, sw - 40, 18),
        kText,
        10.0f,
        true);
    Gdiplus::RectF tg(20, static_cast<float>(height_) - 42, sw - 40, 20);
    const float tg_h = hover_amount(tg);
    draw_text(g, L"t.me/UNIYA0", tg, mix_argb(kTextDim, kText, tg_h), 9.0f, false);
    telegram_hit_ = to_rect(tg);
}

void AppWindow::paint_clips_panel(Gdiplus::Graphics& g, const Gdiplus::RectF& area) {
    using namespace theme;
    fill_round_rect(g, area, 0, kPanel);
    draw_text(
        g,
        tr(L"Последние записи", L"Recent clips"),
        Gdiplus::RectF(area.X + 18, area.Y + 16, 200, 24),
        kText,
        12.0f,
        true);
    Gdiplus::RectF all_r(area.X + area.Width - 80, area.Y + 16, 65, 24);
    const float all_h = hover_amount(all_r);
    draw_text(
        g,
        tr(L"Все ›", L"All ›"),
        Gdiplus::RectF(all_r.X + 3.0f * all_h, all_r.Y, all_r.Width, all_r.Height),
        mix_argb(kTextDim, kText, all_h),
        10.0f,
        false,
        Gdiplus::StringAlignmentFar);
    all_clips_hit_ = to_rect(all_r);

    paint_clip_rows(
        g, Gdiplus::RectF(area.X + 12, area.Y + 52, area.Width - 24, area.Height - 110), 6, kPanel);

    const std::wstring dir = clips_dir_wide();
    Gdiplus::RectF path_r(area.X + 16, area.Y + area.Height - 48, area.Width - 32, 18);
    const float path_h = hover_amount(path_r);
    draw_text(g, dir, path_r, mix_argb(kTextMuted, kTextDim, path_h), 8.0f, false);
    path_hit_ = to_rect(path_r);
    draw_text(
        g,
        free_space_,
        Gdiplus::RectF(area.X + 16, area.Y + area.Height - 28, area.Width - 32, 18),
        kTextDim,
        9.0f,
        false);
}

void AppWindow::paint_clip_rows(
    Gdiplus::Graphics& g, const Gdiplus::RectF& list_area, int max_rows, uint32_t bg) {
    using namespace theme;
    if (clips_.empty()) {
        const float a = page_progress(80.0f, 320.0f);
        draw_text(
            g,
            tr(L"Пока нет клипов.\nХоткей старта → буфер, хоткей сохранения → MP4.",
               L"No clips yet.\nStart hotkey → buffer, save hotkey → MP4."),
            list_area,
            with_alpha(kTextMuted, a),
            10.0f,
            false);
        return;
    }
    const int n = (std::min)(max_rows, static_cast<int>(clips_.size()));
    for (int i = 0; i < n; ++i) {
        const float y = list_area.Y + static_cast<float>(i) * (kClipRowH + 8);
        if (y + kClipRowH > list_area.Y + list_area.Height) {
            break;
        }
        const auto& c = clips_[static_cast<size_t>(i)];
        const Gdiplus::RectF slot(list_area.X, y, list_area.Width, static_cast<float>(kClipRowH));
        const float appear = page_progress(70.0f + 45.0f * static_cast<float>(i), 340.0f);
        const float h = hover_amount(slot);
        Gdiplus::RectF row = slot;
        row.X += (1.0f - appear) * 18.0f;

        fill_round_rect(g, row, 10, mix_argb(kCard, kCardHover, h));
        if (h > 0.01f) {
            draw_round_rect(g, row, 10, with_alpha(0xFF3A3A3A, h), 1.0f);
        }

        // A just-saved clip glows for a moment.
        if (c.path == newest_clip_ && new_clip_ms_ != 0) {
            const float t = static_cast<float>(now_ms_ - new_clip_ms_) / 2200.0f;
            if (t < 1.0f) {
                anim_busy_ = true;
                const float glow = 1.0f - ease_out(t);
                draw_round_rect(g, row, 10, with_alpha(kAccent, glow), 1.6f);
                Gdiplus::RectF badge(row.X + row.Width - 84, row.Y + 8, 40, 16);
                fill_round_rect(g, badge, 8, with_alpha(kAccent, glow));
                draw_text(
                    g,
                    L"NEW",
                    badge,
                    with_alpha(kInk, glow),
                    7.0f,
                    true,
                    Gdiplus::StringAlignmentCenter,
                    Gdiplus::StringAlignmentCenter);
            }
        }

        Gdiplus::RectF thumb(row.X + 10, row.Y + 12, kThumbDrawW, kThumbDrawH);
        fill_round_rect(g, thumb, 8, kThumb);
        if (Gdiplus::Bitmap* preview = thumbnail_for(c.path)) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetClip(thumb, Gdiplus::CombineModeReplace);
            // Gentle zoom on hover.
            Gdiplus::RectF zoomed = thumb;
            zoomed.Inflate(thumb.Width * 0.06f * h, thumb.Height * 0.06f * h);
            g.DrawImage(preview, zoomed);
            g.ResetClip();
        }
        if (h > 0.01f || !thumbnail_for(c.path)) {
            // Play glyph: always for missing thumbnails, fades in over previews on hover.
            const float pa = thumbnail_for(c.path) ? h : 1.0f;
            const float cx = thumb.X + thumb.Width * 0.5f;
            const float cy = thumb.Y + thumb.Height * 0.5f;
            if (thumbnail_for(c.path)) {
                Gdiplus::SolidBrush shade(argb(with_alpha(0xFF000000, 0.45f * pa)));
                g.FillEllipse(&shade, cx - 13.0f, cy - 13.0f, 26.0f, 26.0f);
            }
            const Gdiplus::PointF tri[] = {{cx - 4.0f, cy - 6.5f}, {cx - 4.0f, cy + 6.5f}, {cx + 7.0f, cy}};
            Gdiplus::SolidBrush play(argb(with_alpha(thumbnail_for(c.path) ? kText : kTextDim, pa)));
            g.FillPolygon(&play, tri, 3);
        }

        const float text_w = row.Width - kThumbTextX - 40.0f;
        draw_text(
            g,
            c.name,
            Gdiplus::RectF(row.X + kThumbTextX, row.Y + 12, text_w, 20),
            kText,
            9.5f,
            true);
        draw_text(
            g,
            c.date_display + L"  ·  " + c.size_display,
            Gdiplus::RectF(row.X + kThumbTextX, row.Y + 36, text_w, 18),
            mix_argb(kTextMuted, kTextDim, h),
            8.5f,
            false);
        const Gdiplus::RectF menu(slot.X + slot.Width - 36, slot.Y + 20, 28, 28);
        const float mh = hover_amount(menu);
        if (mh > 0.01f) {
            fill_round_rect(g, Gdiplus::RectF(menu.X + (row.X - slot.X), menu.Y, menu.Width, menu.Height), 8, with_alpha(kBorder, mh));
        }
        draw_text(
            g,
            L"⋯",
            Gdiplus::RectF(menu.X + (row.X - slot.X), menu.Y, menu.Width, menu.Height),
            mix_argb(kTextDim, kText, (std::max)(h * 0.5f, mh)),
            14.0f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);

        if (appear < 1.0f) {
            Gdiplus::RectF veil = row;
            veil.Inflate(2.0f, 2.0f);
            fill_round_rect(g, veil, 11, with_alpha(bg, 1.0f - appear));
        }

        ClipHit hit;
        hit.row = to_rect(slot);
        hit.menu = to_rect(menu);
        hit.index = i;
        clip_hits_.push_back(hit);
    }
}

void AppWindow::paint_buffer_meter(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float appear) {
    using namespace theme;
    const bool armed = capture_ && capture_->is_armed();
    const float cap = static_cast<float>((std::max)(1, settings_.buffer_sec));
    const float have = armed
        ? (std::min)(cap, static_cast<float>(now_ms_ - armed_since_ms_) / 1000.0f)
        : 0.0f;
    const float fill = smooth(anim_key("meter.fill"), have / cap, 7.0f, 0.0008f);
    const float live = smooth(anim_key("meter.live"), armed ? 1.0f : 0.0f, 6.0f);

    fill_round_rect(g, r, 14, kCard);
    draw_round_rect(g, r, 14, mix_argb(kBorder, 0xFF3A3A3A, live), 1.0f);

    const uint32_t text = with_alpha(kText, appear);
    draw_text(g, tr(L"Буфер", L"Buffer"), Gdiplus::RectF(r.X + 18, r.Y + 12, 120, 22), text, 11.0f, true);

    std::wstring state;
    if (!armed) {
        state = tr(L"выключен — ", L"off — ") + utf8(ir::settings::hotkey_label(settings_.hk_arm)) +
                tr(L" чтобы начать", L" to start");
    } else if (have < cap) {
        state = tr(L"заполняется…", L"filling…");
    } else {
        state = tr(L"готов — ", L"ready — ") + utf8(ir::settings::hotkey_label(settings_.hk_save)) +
                tr(L" сохранит клип", L" saves a clip");
    }
    draw_text(
        g,
        state,
        Gdiplus::RectF(r.X + 92, r.Y + 14, r.Width - 200, 20),
        with_alpha(kTextDim, appear),
        9.5f,
        false);

    const std::wstring unit = tr(L" с", L" s");
    const std::wstring amount = armed
        ? std::to_wstring(static_cast<int>(have)) + L" / " + std::to_wstring(static_cast<int>(cap)) + unit
        : std::to_wstring(static_cast<int>(cap)) + unit;
    draw_text(
        g,
        amount,
        Gdiplus::RectF(r.X + r.Width - 120, r.Y + 13, 102, 20),
        with_alpha(mix_argb(kTextDim, kText, live), appear),
        10.0f,
        true,
        Gdiplus::StringAlignmentFar);

    const Gdiplus::RectF bar(r.X + 18, r.Y + r.Height - 24, r.Width - 36, 8);
    fill_round_rect(g, bar, 4, kNavActive);
    const float fill_w = bar.Width * clamp01f(fill);
    if (fill_w > 1.0f) {
        const Gdiplus::RectF done(bar.X, bar.Y, (std::max)(8.0f, fill_w), bar.Height);
        Gdiplus::GraphicsPath path;
        const float d = bar.Height;
        path.AddArc(done.X, done.Y, d, d, 90, 180);
        path.AddArc(done.X + done.Width - d, done.Y, d, d, 270, 180);
        path.CloseFigure();
        Gdiplus::LinearGradientBrush grad(
            Gdiplus::PointF(bar.X - 1.0f, 0.0f),
            Gdiplus::PointF(bar.X + bar.Width + 1.0f, 0.0f),
            argb(0xFF5A5A5A),
            argb(kAccent));
        g.FillPath(&grad, &path);

        if (armed) {
            // Light sweep travelling along the filled part.
            const float span = done.Width + 120.0f;
            const float sx = done.X - 60.0f + std::fmod(static_cast<float>(now_ms_) * 0.12f, span);
            Gdiplus::LinearGradientBrush sweep(
                Gdiplus::PointF(sx - 40.0f, 0.0f),
                Gdiplus::PointF(sx + 40.0f, 0.0f),
                argb(0x00FFFFFF),
                argb(0x00FFFFFF));
            const Gdiplus::Color colors[] = {argb(0x00FFFFFF), argb(0x88FFFFFF), argb(0x00FFFFFF)};
            const Gdiplus::REAL positions[] = {0.0f, 0.5f, 1.0f};
            sweep.SetInterpolationColors(colors, positions, 3);
            g.SetClip(&path, Gdiplus::CombineModeReplace);
            g.FillRectangle(&sweep, sx - 40.0f, bar.Y, 80.0f, bar.Height);
            g.ResetClip();

            const float hx = done.X + done.Width - bar.Height * 0.5f;
            const float hy = bar.Y + bar.Height * 0.5f;
            Gdiplus::GraphicsPath halo;
            halo.AddEllipse(hx - 9.0f, hy - 9.0f, 18.0f, 18.0f);
            Gdiplus::PathGradientBrush glow(&halo);
            glow.SetCenterColor(argb(with_alpha(0xFFFFFFFF, 0.55f * live)));
            Gdiplus::Color clear = argb(0x00FFFFFF);
            INT one = 1;
            glow.SetSurroundColors(&clear, &one);
            g.FillPath(&glow, &halo);
        }
    }
    // Ten-second ticks.
    const int step = cap > 40.0f ? 10 : 5;
    Gdiplus::Pen tick(argb(0x55000000), 1.0f);
    for (int s = step; s < static_cast<int>(cap); s += step) {
        const float x = bar.X + bar.Width * (static_cast<float>(s) / cap);
        g.DrawLine(&tick, x, bar.Y + 1.0f, x, bar.Y + bar.Height - 1.0f);
    }
}

void AppWindow::paint_home(Gdiplus::Graphics& g) {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    const float right_w = static_cast<float>(kRightPanelW);
    const float mid_w = static_cast<float>(width_) - left - right_w;
    const bool armed = capture_ && capture_->is_armed();

    const float a0 = page_progress(0.0f, 420.0f);
    const float mark = 84.0f;
    const Gdiplus::RectF logo(left + mid_w * 0.5f - mark * 0.5f, 36.0f + (1.0f - a0) * 8.0f, mark, mark);
    const float live = smooth(anim_key("logo.live"), armed ? 1.0f : 0.0f, 5.0f);
    if (live > 0.01f) {
        const float cx = logo.X + mark * 0.5f;
        const float cy = logo.Y + mark * 0.5f;
        const float breath = 0.85f + 0.15f * std::sin(static_cast<float>(now_ms_) * 0.003f);
        const float hr = mark * 0.95f;
        Gdiplus::GraphicsPath halo;
        halo.AddEllipse(cx - hr, cy - hr, hr * 2.0f, hr * 2.0f);
        Gdiplus::PathGradientBrush glow(&halo);
        glow.SetCenterColor(argb(with_alpha(0xFFFFFFFF, 0.10f * live * breath)));
        Gdiplus::Color clear = argb(0x00FFFFFF);
        INT one = 1;
        glow.SetSurroundColors(&clear, &one);
        g.FillPath(&glow, &halo);

        Gdiplus::RectF orbit = logo;
        orbit.Inflate(7.0f, 7.0f);
        const float ang = std::fmod(static_cast<float>(now_ms_) * 0.2f, 360.0f);
        Gdiplus::Pen arc(argb(with_alpha(kAccent, live)), 2.4f);
        arc.SetStartCap(Gdiplus::LineCapRound);
        arc.SetEndCap(Gdiplus::LineCapRound);
        g.DrawArc(&arc, orbit, ang, 80.0f * live);
        Gdiplus::Pen tail(argb(with_alpha(kAccent, 0.35f * live)), 1.6f);
        tail.SetStartCap(Gdiplus::LineCapRound);
        tail.SetEndCap(Gdiplus::LineCapRound);
        g.DrawArc(&tail, orbit, ang + 180.0f, 34.0f * live);
    }
    draw_brand_uo_ring(g, logo, with_alpha(kText, a0));

    const float a1 = page_progress(60.0f, 420.0f);
    draw_text(
        g,
        L"UniyaOver",
        Gdiplus::RectF(left + 40, 128 + (1.0f - a1) * 8.0f, mid_w - 80, 36),
        with_alpha(kText, a1),
        22.0f,
        true,
        Gdiplus::StringAlignmentCenter);

    const std::wstring secs = std::to_wstring(settings_.buffer_sec);
    const std::wstring desc =
        tr(L"Простое приложение для записи последних ", L"A simple app that saves the last ") + secs +
        tr(L" секунд\nэкрана/игры по горячей клавише.", L" seconds\nof your screen or game with a hotkey.");
    const float a2 = page_progress(110.0f, 420.0f);
    draw_text(
        g,
        desc,
        Gdiplus::RectF(left + 50, 168 + (1.0f - a2) * 8.0f, mid_w - 100, 52),
        with_alpha(kTextDim, a2),
        10.5f,
        false,
        Gdiplus::StringAlignmentCenter);

    const float a3 = page_progress(160.0f, 420.0f);
    paint_buffer_meter(g, Gdiplus::RectF(left + 48, 232 + (1.0f - a3) * 10.0f, mid_w - 96, 74), a3);

    // Feature trio with tiny monochrome glyphs.
    struct Feature {
        const wchar_t* title;
        const wchar_t* sub;
    };
    const Feature features[] = {
        {tr(L"Мгновенно", L"Instant"), tr(L"Запуск по хоткею", L"Starts with a hotkey")},
        {tr(L"Кольцевой буфер", L"Ring buffer"), tr(L"Сохраняет нужный момент", L"Keeps the moment")},
        {tr(L"Минимализм", L"Minimal"), tr(L"Ничего лишнего", L"Nothing extra")},
    };
    const float col_w = (mid_w - 96) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float a = page_progress(220.0f + 70.0f * static_cast<float>(i), 420.0f);
        const float x = left + 48 + col_w * static_cast<float>(i);
        const float y = 326 + (1.0f - a) * 12.0f;
        const float cx = x + col_w * 0.5f;
        const float cy = y + 15.0f;
        Gdiplus::SolidBrush disc(argb(with_alpha(kNavActive, a)));
        g.FillEllipse(&disc, cx - 15.0f, cy - 15.0f, 30.0f, 30.0f);
        Gdiplus::Pen pen(argb(with_alpha(kText, a)), 1.6f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        if (i == 0) {
            const Gdiplus::PointF bolt[] = {
                {cx + 1.5f, cy - 7.0f}, {cx - 3.5f, cy + 1.0f}, {cx + 0.5f, cy + 1.0f}, {cx - 1.5f, cy + 7.0f}, {cx + 3.5f, cy - 1.0f}, {cx - 0.5f, cy - 1.0f}};
            Gdiplus::SolidBrush fill(argb(with_alpha(kText, a)));
            g.FillPolygon(&fill, bolt, 6);
        } else if (i == 1) {
            const float spin = armed ? std::fmod(static_cast<float>(now_ms_) * 0.25f, 360.0f) : -60.0f;
            g.DrawArc(&pen, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f, spin, 290.0f);
            const float tip = (spin + 290.0f) * 3.14159265f / 180.0f;
            Gdiplus::SolidBrush dot(argb(with_alpha(kText, a)));
            g.FillEllipse(&dot, cx + 7.0f * std::cos(tip) - 2.0f, cy + 7.0f * std::sin(tip) - 2.0f, 4.0f, 4.0f);
        } else {
            g.DrawLine(&pen, cx - 6.0f, cy - 4.0f, cx + 6.0f, cy - 4.0f);
            g.DrawLine(&pen, cx - 6.0f, cy, cx + 3.0f, cy);
            g.DrawLine(&pen, cx - 6.0f, cy + 4.0f, cx + 0.0f, cy + 4.0f);
        }
        draw_text(
            g,
            features[i].title,
            Gdiplus::RectF(x, y + 36, col_w, 20),
            with_alpha(kText, a),
            10.5f,
            true,
            Gdiplus::StringAlignmentCenter);
        draw_text(
            g,
            features[i].sub,
            Gdiplus::RectF(x, y + 56, col_w, 18),
            with_alpha(kTextMuted, a),
            8.5f,
            false,
            Gdiplus::StringAlignmentCenter);
    }

    Gdiplus::RectF card(left + 48, static_cast<float>(height_) - 236, mid_w - 96, 186);
    fill_round_rect(g, card, 14, kCard);
    draw_round_rect(g, card, 14, kBorder, 1.0f);
    draw_text(
        g,
        tr(L"Горячие клавиши", L"Hotkeys"),
        Gdiplus::RectF(card.X + 20, card.Y + 14, card.Width - 40, 22),
        kText,
        11.0f,
        true,
        Gdiplus::StringAlignmentCenter);

    auto hk_pill = [&](int index, float y, const std::wstring& keys, const wchar_t* action) {
        const Gdiplus::RectF pill(card.X + 20, y, card.Width - 40, 28);
        const float h = hover_amount(pill);
        float flash = 0.0f;
        if (flash_ms_[index] != 0) {
            const float t = static_cast<float>(now_ms_ - flash_ms_[index]) / 900.0f;
            if (t < 1.0f) {
                anim_busy_ = true;
                flash = 1.0f - ease_out(t);
            }
        }
        fill_round_rect(g, pill, 14, mix_argb(mix_argb(kNavActive, kCardHover, h), kAccent, flash));
        draw_text(
            g,
            keys + L"   —   " + action,
            Gdiplus::RectF(pill.X + 12, pill.Y, pill.Width - 24, pill.Height),
            mix_argb(kText, kInk, flash),
            9.5f,
            false,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
        home_hk_hit_[index] = to_rect(pill);
    };
    hk_pill(0, card.Y + 44, utf8(ir::settings::hotkey_label(settings_.hk_arm)), tr(L"старт буфера", L"start buffer"));
    hk_pill(1, card.Y + 76, utf8(ir::settings::hotkey_label(settings_.hk_save)), tr(L"весь экран", L"full screen"));
    hk_pill(2, card.Y + 108, utf8(ir::settings::hotkey_label(settings_.hk_region)), tr(L"область", L"region"));
    hk_pill(3, card.Y + 140, utf8(ir::settings::hotkey_label(settings_.hk_stop)), tr(L"стоп буфера", L"stop buffer"));

    paint_clips_panel(g, Gdiplus::RectF(left + mid_w, 0, right_w, static_cast<float>(height_)));
}

void AppWindow::paint_library(Gdiplus::Graphics& g) {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    Gdiplus::RectF area(left, 0, static_cast<float>(width_) - left, static_cast<float>(height_));
    draw_text(g, tr(L"Библиотека", L"Library"), Gdiplus::RectF(left + 28, 24, 300, 32), kText, 18.0f, true);
    const std::wstring dir = clips_dir_wide();
    const Gdiplus::RectF path_r(left + 28, 58, area.Width - 56, 20);
    const float path_h = hover_amount(path_r);
    draw_text(g, dir + L"   ·   " + free_space_, path_r, mix_argb(kTextMuted, kTextDim, path_h), 9.0f, false);
    path_hit_ = to_rect(path_r);
    paint_clip_rows(g, Gdiplus::RectF(left + 24, 96, area.Width - 48, area.Height - 120), 12, kBg);
}

void AppWindow::paint_settings(Gdiplus::Graphics& g) {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    Gdiplus::RectF area(left, 0, static_cast<float>(width_) - left, static_cast<float>(height_));

    draw_text(g, tr(L"Настройки", L"Settings"), Gdiplus::RectF(left + 28, 18, 300, 32), kText, 18.0f, true);
    {
        std::wstring hw_line = ir::hw::status_line(hw_);
        if (hw_.capture_ready) {
            const auto resolved = ir::hw::resolve_codec(settings_, hw_);
            hw_line += tr(L" · кодек ", L" · codec ");
            hw_line += (resolved == ir::hw::VideoCodec::Hevc) ? L"HEVC" : L"H.264";
            if (settings_.video_codec == "auto") {
                hw_line += tr(L" (авто)", L" (auto)");
            }
        }
        draw_text(
            g,
            hw_line,
            Gdiplus::RectF(left + 28, 54, area.Width - 56, 20),
            hw_.capture_ready ? kTextDim : 0xFFC07070,
            9.0f,
            false);
    }
    draw_text(
        g,
        tr(L"Сохраняются на диск. FPS/устройства — со следующего старта буфера.",
           L"Saved to disk. FPS/devices apply from the next buffer start."),
        Gdiplus::RectF(left + 28, 74, area.Width - 56, 18),
        kTextMuted,
        9.0f,
        false);

    // Tabs with a sliding accent pill.
    const float tab_y = 104.0f;
    const float tab_gap = 8.0f;
    const float tab_w = 118.0f;
    const float tab_x0 = left + 28;
    const wchar_t* tab_labels[] = {
        tr(L"Качество", L"Quality"),
        tr(L"Хоткеи", L"Hotkeys"),
        tr(L"Устройства", L"Devices"),
        tr(L"Клипы", L"Clips"),
        tr(L"Уведомление", L"Notification"),
    };
    RECT* tab_hits[] = {&tab_quality_, &tab_hotkeys_, &tab_devices_, &tab_clips_, &tab_notify_};
    const int active_tab = static_cast<int>(settings_tab_);
    for (int i = 0; i < 5; ++i) {
        const Gdiplus::RectF r(tab_x0 + (tab_w + tab_gap) * static_cast<float>(i), tab_y, tab_w, 34);
        const float h = i == active_tab ? 0.0f : hover_amount(r);
        fill_round_rect(g, r, 10, mix_argb(kCard, kCardHover, h));
        *tab_hits[i] = to_rect(r);
    }
    const float pill_x = smooth(
        anim_key("tab.pill"), tab_x0 + (tab_w + tab_gap) * static_cast<float>(active_tab), 16.0f, 0.3f);
    fill_round_rect(g, Gdiplus::RectF(pill_x, tab_y, tab_w, 34), 10, kAccent);
    for (int i = 0; i < 5; ++i) {
        const Gdiplus::RectF r(tab_x0 + (tab_w + tab_gap) * static_cast<float>(i), tab_y, tab_w, 34);
        const float cover = clamp01f(1.0f - std::fabs(pill_x - r.X) / (tab_w + tab_gap));
        draw_text(
            g,
            tab_labels[i],
            r,
            mix_argb(kTextDim, kInk, cover),
            10.0f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
    }

    float y = tab_y + 52.0f;

    // Selectable / pressable button with animated selection and hover.
    auto button = [&](const Gdiplus::RectF& r, const std::wstring& label, bool on, bool allowed, RECT& hit, float font_pt = 10.5f) {
        const float a = smooth(rect_key(r) ^ 0x9E3779B9u, on ? 1.0f : 0.0f, 16.0f);
        const float h = allowed ? hover_amount(r) : 0.0f;
        uint32_t fill = mix_argb(mix_argb(kCard, kCardHover, h), kAccent, a);
        fill = mix_argb(fill, 0xFFFFFFFF, a * h * 0.5f);
        fill_round_rect(g, r, 10, fill);
        draw_round_rect(g, r, 10, allowed ? mix_argb(kBorder, 0xFF3C3C3C, h) : 0xFF333333, 1.0f);
        draw_text(
            g,
            label,
            r,
            allowed ? mix_argb(kText, kInk, a) : kTextMuted,
            font_pt,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
        hit = allowed ? to_rect(r) : RECT{};
    };
    auto label = [&](float x, float w, const std::wstring& text, bool dim = false) {
        draw_text(g, text, Gdiplus::RectF(x, y, w, 20), dim ? kTextMuted : kText, 11.0f, true);
    };
    // Clickable full-width card with a chevron.
    auto chooser = [&](const std::wstring& text, float font_pt, uint32_t color, RECT& hit) {
        const Gdiplus::RectF card(left + 28, y, (std::min)(area.Width - 56, 520.0f), 40);
        const float h = hover_amount(card);
        fill_round_rect(g, card, 10, mix_argb(kCard, kCardHover, h));
        draw_round_rect(g, card, 10, with_alpha(0xFF3A3A3A, h), 1.0f);
        draw_text(
            g,
            text,
            Gdiplus::RectF(card.X + 14, card.Y, card.Width - 52, card.Height),
            color,
            font_pt,
            false,
            Gdiplus::StringAlignmentNear,
            Gdiplus::StringAlignmentCenter);
        draw_text(
            g,
            L"›",
            Gdiplus::RectF(card.X + card.Width - 34 + 4.0f * h, card.Y, 20, card.Height),
            mix_argb(kTextMuted, kText, h),
            13.0f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
        hit = to_rect(card);
    };

    if (settings_tab_ == SettingsTab::Notify) {
        const bool enabled = settings_.toast_enabled;
        label(left + 28, 400, tr(L"Уведомление о сохранении", L"Save notification"));
        y += 26;
        button(Gdiplus::RectF(left + 28, y, 90, 34), tr(L"Вкл", L"On"), enabled, true, toast_on_hit_);
        button(Gdiplus::RectF(left + 126, y, 90, 34), tr(L"Выкл", L"Off"), !enabled, true, toast_off_hit_);

        y += 52;
        label(left + 28, 200, tr(L"Размер", L"Size"), !enabled);
        y += 26;
        const wchar_t* size_labels[] = {
            tr(L"Маленький", L"Small"), tr(L"Средний", L"Medium"), tr(L"Большой", L"Large")};
        for (int i = 0; i < 3; ++i) {
            button(
                Gdiplus::RectF(left + 28 + 128.0f * static_cast<float>(i), y, 120, 34),
                size_labels[i],
                settings_.toast_size == i,
                true,
                toast_size_hit_[i]);
        }

        y += 52;
        label(left + 28, 300, tr(L"Положение на экране", L"Position on screen"), !enabled);
        y += 26;

        Gdiplus::RectF screen(left + 28, y, 272, 153);
        fill_round_rect(g, screen, 12, kCard);
        draw_round_rect(g, screen, 12, kBorder, 1.0f);
        {
            Gdiplus::SolidBrush stand(argb(kBorder));
            g.FillRectangle(&stand, screen.X + screen.Width * 0.5f - 14, screen.GetBottom(), 28.0f, 10.0f);
            fill_round_rect(
                g,
                Gdiplus::RectF(screen.X + screen.Width * 0.5f - 44, screen.GetBottom() + 10, 88, 5),
                2.5f,
                kBorder);
        }

        constexpr float pill_w = 64.0f;
        constexpr float pill_h = 20.0f;
        constexpr float inset = 12.0f;
        const uint32_t sel_color = enabled ? kAccent : kTextMuted;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3;
            const int row = i / 3;
            const float px = col == 0 ? screen.X + inset
                           : col == 1 ? screen.X + (screen.Width - pill_w) * 0.5f
                                      : screen.GetRight() - inset - pill_w;
            const float py = row == 0 ? screen.Y + inset : screen.GetBottom() - inset - pill_h;
            const Gdiplus::RectF pill(px, py, pill_w, pill_h);
            Gdiplus::RectF hit_r = pill;
            hit_r.Inflate(6.0f, 6.0f);
            const float h = hover_amount(hit_r);
            const float a = smooth(anim_key("toast.pos", i), settings_.toast_position == i ? 1.0f : 0.0f, 14.0f);

            fill_round_rect(g, pill, pill_h * 0.5f, mix_argb(mix_argb(0xFF222222, 0xFF2E2E2E, h), sel_color, a));
            if (a < 0.99f) {
                draw_round_rect(g, pill, pill_h * 0.5f, with_alpha(mix_argb(0xFF3A3A3A, 0xFF5A5A5A, h), 1.0f - a), 1.0f);
            }
            if (a > 0.01f) {
                const float cx = pill.X + 10.0f;
                const float cy = pill.Y + pill_h * 0.5f;
                Gdiplus::SolidBrush dot(argb(with_alpha(kInk, a)));
                const float dr = 6.0f * (0.6f + 0.4f * a);
                g.FillEllipse(&dot, cx - dr, cy - dr, dr * 2.0f, dr * 2.0f);
                Gdiplus::Pen check(argb(with_alpha(sel_color, a)), 1.6f);
                check.SetStartCap(Gdiplus::LineCapRound);
                check.SetEndCap(Gdiplus::LineCapRound);
                check.SetLineJoin(Gdiplus::LineJoinRound);
                const Gdiplus::PointF mark[] = {{cx - 2.6f, cy + 0.2f}, {cx - 0.6f, cy + 2.2f}, {cx + 2.8f, cy - 2.0f}};
                g.DrawLines(&check, mark, 3);
                fill_round_rect(g, Gdiplus::RectF(pill.X + 21, cy - 4.0f, 32.0f * a, 3), 1.5f, with_alpha(kInk, a));
                fill_round_rect(g, Gdiplus::RectF(pill.X + 21, cy + 1.5f, 22.0f * a, 2.5f), 1.25f, with_alpha(0xFF4A4A4A, a));
            }
            toast_pos_hit_[i] = to_rect(hit_r);
        }

        const float side_x = screen.GetRight() + 28;
        const float side_w = (std::max)(200.0f, (std::min)(340.0f, area.GetRight() - 28 - side_x));
        if (enabled) {
            button(Gdiplus::RectF(side_x, y, 190, 34), tr(L"Показать пример", L"Show preview"), true, true, toast_preview_hit_);
        }
        draw_text(
            g,
            enabled ? tr(L"Пример появится на мониторе, где открыто это окно.",
                         L"The preview appears on the monitor with this window.")
                    : tr(L"Уведомления выключены — сохранение работает молча.",
                         L"Notifications are off — clips save silently."),
            Gdiplus::RectF(side_x, y + 48, side_w, 40),
            kTextDim,
            9.5f,
            false);
        draw_text(
            g,
            tr(L"В клипы уведомление не попадает. Поверх игр в эксклюзивном полноэкранном "
               L"режиме Windows его не покажет — выбирайте «оконный без рамки».",
               L"The notification never appears in your clips. Windows can't show it over "
               L"exclusive fullscreen games — use borderless windowed mode."),
            Gdiplus::RectF(side_x, y + 92, side_w, 64),
            kTextMuted,
            9.0f,
            false);
        return;
    }

    if (settings_tab_ == SettingsTab::Quality) {
        label(left + 28, 200, tr(L"Кодек", L"Codec"));
        y += 26;
        const bool hevc_ok = ir::hw::hevc_available(hw_);
        button(Gdiplus::RectF(left + 28, y, 90, 34), tr(L"Авто", L"Auto"), settings_.video_codec == "auto", true, codec_auto_);
        button(Gdiplus::RectF(left + 126, y, 90, 34), L"H.264", settings_.video_codec == "h264", true, codec_h264_);
        button(Gdiplus::RectF(left + 224, y, 90, 34), L"HEVC", settings_.video_codec == "hevc", hevc_ok, codec_hevc_);
        if (!hevc_ok) {
            draw_text(
                g,
                tr(L"HEVC NVENC недоступен на этом ПК", L"HEVC NVENC isn't available on this PC"),
                Gdiplus::RectF(left + 328, y + 8, 320, 20),
                kTextMuted,
                9.0f,
                false);
        }

        y += 52;
        label(left + 28, 200, L"FPS");
        y += 26;
        auto fps_btn = [&](float x, int fps, RECT& hit) {
            const bool allowed = ir::hw::fps_allowed(hw_, fps);
            button(Gdiplus::RectF(x, y, 80, 34), std::to_wstring(fps), settings_.fps_cap == fps && allowed, allowed, hit, 11.0f);
        };
        fps_btn(left + 28, 60, fps60_);
        fps_btn(left + 118, 144, fps144_);
        fps_btn(left + 208, 240, fps240_);
        if (!hw_.capture_ready) {
            draw_text(
                g,
                tr(L"NVENC недоступен — FPS нельзя выбрать", L"NVENC unavailable — FPS can't be changed"),
                Gdiplus::RectF(left + 28, y + 42, 520, 22),
                0xFFC07070,
                9.0f,
                false);
        } else if (hw_.nvenc.recommended_max_fps < 240) {
            draw_text(
                g,
                tr(L"Рекомендуется до ", L"Recommended up to ") + std::to_wstring(hw_.nvenc.recommended_max_fps) +
                    tr(L" FPS для этой карты", L" FPS for this GPU"),
                Gdiplus::RectF(left + 28, y + 42, 520, 22),
                kTextMuted,
                9.0f,
                false);
        }

        y += 70;
        label(left + 28, 220, tr(L"Битрейт (Mbps)", L"Bitrate (Mbps)"));
        draw_text(g, tr(L"Буфер (сек)", L"Buffer (sec)"), Gdiplus::RectF(left + 280, y, 200, 20), kText, 11.0f, true);
        y += 26;
        auto stepper = [&](float x, const std::wstring& val, RECT& minus, RECT& plus) {
            const Gdiplus::RectF v(x + 42, y, 70, 34);
            button(Gdiplus::RectF(x, y, 36, 34), L"−", false, true, minus, 14.0f);
            fill_round_rect(g, v, 10, kCard);
            draw_text(
                g, val, v, kText, 12.0f, true, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
            button(Gdiplus::RectF(x + 118, y, 36, 34), L"+", false, true, plus, 14.0f);
        };
        stepper(left + 28, std::to_wstring(bitrate_mbps(settings_)), bitrate_minus_, bitrate_plus_);
        stepper(left + 280, std::to_wstring(settings_.buffer_sec), buffer_minus_, buffer_plus_);
        return;
    }

    if (settings_tab_ == SettingsTab::Hotkeys) {
        draw_text(
            g,
            tr(L"Клик по полю, затем Ctrl+X или Ctrl+Shift+R… (Esc — отмена)",
               L"Click a field, then press Ctrl+X or Ctrl+Shift+R… (Esc — cancel)"),
            Gdiplus::RectF(left + 28, y, 620, 20),
            kTextMuted,
            9.5f,
            false);
        y += 28;
        auto hk_row = [&](float yy, const wchar_t* title, const ir::settings::HotkeyBinding& b, bool listening, RECT& hit) {
            draw_text(g, title, Gdiplus::RectF(left + 28, yy + 6, 120, 24), kTextDim, 10.0f, false);
            const Gdiplus::RectF pill(left + 160, yy, 280, 34);
            const float h = hover_amount(pill);
            const float a = smooth(anim_key("hk.listen", static_cast<int>(yy)), listening ? 1.0f : 0.0f, 16.0f);
            fill_round_rect(g, pill, 10, mix_argb(mix_argb(kCard, kCardHover, h), kAccent, a));
            if (listening) {
                const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_ms_) * 0.008f);
                Gdiplus::RectF ring = pill;
                ring.Inflate(2.0f + 2.0f * pulse, 2.0f + 2.0f * pulse);
                draw_round_rect(g, ring, 12, with_alpha(kAccent, 0.25f + 0.35f * (1.0f - pulse)), 1.2f);
            }
            std::wstring text;
            if (listening) {
                if (listen_mods_ == 0) {
                    text = tr(L"Жду сочетание…", L"Waiting for keys…");
                } else {
                    ir::settings::HotkeyBinding preview;
                    preview.modifiers = listen_mods_;
                    preview.vk = 0;
                    text = utf8(ir::settings::hotkey_label(preview)) + L"…";
                }
            } else {
                text = utf8(ir::settings::hotkey_label(b));
            }
            draw_text(
                g,
                text,
                pill,
                mix_argb(kText, kInk, a),
                10.5f,
                true,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
            hit = to_rect(pill);
        };
        hk_row(y, tr(L"Старт", L"Start"), settings_.hk_arm, listen_ == ListenTarget::Arm, hk_arm_hit_);
        y += 42;
        hk_row(y, tr(L"Весь экран", L"Full screen"), settings_.hk_save, listen_ == ListenTarget::Save, hk_save_hit_);
        y += 42;
        hk_row(y, tr(L"Область", L"Region"), settings_.hk_region, listen_ == ListenTarget::Region, hk_region_hit_);
        y += 42;
        hk_row(y, tr(L"Стоп", L"Stop"), settings_.hk_stop, listen_ == ListenTarget::Stop, hk_stop_hit_);
        return;
    }

    if (settings_tab_ == SettingsTab::Devices) {
        label(left + 28, 400, tr(L"Экран (клик — следующий)", L"Display (click for next)"));
        y += 26;
        std::wstring mon_label = tr(L"Основной монитор", L"Primary monitor");
        if (settings_.monitor_index >= 0) {
            bool found = false;
            for (const auto& m : monitors_) {
                if (m.index == settings_.monitor_index) {
                    mon_label = m.label;
                    found = true;
                    break;
                }
            }
            if (!found) {
                mon_label = tr(L"Монитор #", L"Monitor #") + std::to_wstring(settings_.monitor_index);
            }
        }
        chooser(mon_label, 10.5f, kText, monitor_hit_);

        y += 58;
        label(left + 28, 400, tr(L"Звук (клик — следующее устройство)", L"Audio (click for next device)"));
        y += 26;
        std::wstring aud_label = tr(L"Авто (активное воспроизведение)", L"Auto (active playback)");
        if (!settings_.audio_device_id.empty()) {
            aud_label = tr(L"Устройство не найдено", L"Device not found");
            for (const auto& a : audio_devices_) {
                if (a.id == settings_.audio_device_id) {
                    aud_label = a.label;
                    break;
                }
            }
        }
        chooser(aud_label, 10.0f, kText, audio_hit_);
        return;
    }

    // Clips tab
    label(left + 28, 300, tr(L"Папка клипов", L"Clips folder"));
    y += 26;
    chooser(clips_dir_wide(), 9.5f, kTextDim, path_hit_);

    y += 52;
    button(Gdiplus::RectF(left + 28, y, 160, 34), tr(L"Выбрать…", L"Choose…"), true, true, clips_browse_hit_);
    button(
        Gdiplus::RectF(left + 200, y, 220, 34),
        tr(L"Сбросить по умолчанию", L"Reset to default"),
        false,
        true,
        clips_reset_hit_,
        10.0f);
}

void AppWindow::start_onboarding() {
    cancel_hotkey_listen();
    tour_step_ = TourStep::Welcome;
    prepare_onboarding_step();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::finish_onboarding(bool mark_done) {
    tour_step_ = TourStep::None;
    tour_spotlight_ = {};
    if (mark_done) {
        settings_.onboarding_done = true;
        apply_and_save();
    }
    page_ = Page::Home;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int AppWindow::onboarding_step_count() const {
    return 7;  // Welcome..Finish
}

int AppWindow::onboarding_step_index() const {
    switch (tour_step_) {
    case TourStep::Welcome:
        return 1;
    case TourStep::Idea:
        return 2;
    case TourStep::Hotkeys:
        return 3;
    case TourStep::Clips:
        return 4;
    case TourStep::Nav:
        return 5;
    case TourStep::Settings:
        return 6;
    case TourStep::Finish:
        return 7;
    default:
        return 0;
    }
}

void AppWindow::prepare_onboarding_step() {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    const float right_w = static_cast<float>(kRightPanelW);
    const float mid_w = static_cast<float>(width_) - left - right_w;

    tour_spotlight_ = {};
    switch (tour_step_) {
    case TourStep::Welcome:
        page_ = Page::Home;
        break;
    case TourStep::Idea:
        page_ = Page::Home;
        tour_spotlight_ = Gdiplus::RectF(left + 40, 224, mid_w - 80, 90);
        break;
    case TourStep::Hotkeys:
        page_ = Page::Home;
        tour_spotlight_ = Gdiplus::RectF(left + 48, static_cast<float>(height_) - 236, mid_w - 96, 186);
        break;
    case TourStep::Clips:
        page_ = Page::Home;
        tour_spotlight_ = Gdiplus::RectF(left + mid_w, 0, right_w, static_cast<float>(height_));
        break;
    case TourStep::Nav:
        page_ = Page::Home;
        tour_spotlight_ = Gdiplus::RectF(8, 84, static_cast<float>(kSidebarW) - 16, 160);
        break;
    case TourStep::Settings:
        page_ = Page::Settings;
        settings_tab_ = SettingsTab::Quality;
        refresh_device_lists();
        tour_spotlight_ = Gdiplus::RectF(left + 20, 100, (std::min)(520.0f, static_cast<float>(width_) - left - 40), 280);
        break;
    case TourStep::Finish:
        page_ = Page::Home;
        break;
    default:
        break;
    }
}

void AppWindow::onboarding_next() {
    switch (tour_step_) {
    case TourStep::Welcome:
        tour_step_ = TourStep::Idea;
        break;
    case TourStep::Idea:
        tour_step_ = TourStep::Hotkeys;
        break;
    case TourStep::Hotkeys:
        tour_step_ = TourStep::Clips;
        break;
    case TourStep::Clips:
        tour_step_ = TourStep::Nav;
        break;
    case TourStep::Nav:
        tour_step_ = TourStep::Settings;
        break;
    case TourStep::Settings:
        tour_step_ = TourStep::Finish;
        break;
    case TourStep::Finish:
        finish_onboarding(true);
        return;
    default:
        finish_onboarding(true);
        return;
    }
    prepare_onboarding_step();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool AppWindow::handle_onboarding_click(int x, int y) {
    if (point_in_rect(x, y, tour_next_hit_)) {
        onboarding_next();
        return true;
    }
    if (point_in_rect(x, y, tour_skip_hit_)) {
        finish_onboarding(true);
        return true;
    }
    return true;  // swallow other clicks while tour is open
}

void AppWindow::paint_onboarding(Gdiplus::Graphics& g) {
    using namespace theme;

    const float since = static_cast<float>(now_ms_ - tour_enter_ms_);
    const bool has_spot = tour_spotlight_.Width > 1.0f && tour_spotlight_.Height > 1.0f;
    Gdiplus::RectF spot;
    if (has_spot) {
        spot = Gdiplus::RectF(
            smooth(anim_key("tour.spot.x"), tour_spotlight_.X, 12.0f, 0.3f),
            smooth(anim_key("tour.spot.y"), tour_spotlight_.Y, 12.0f, 0.3f),
            smooth(anim_key("tour.spot.w"), tour_spotlight_.Width, 12.0f, 0.3f),
            smooth(anim_key("tour.spot.h"), tour_spotlight_.Height, 12.0f, 0.3f));
        g.SetClip(spot, Gdiplus::CombineModeExclude);
    }
    Gdiplus::SolidBrush dim(argb(0xCC0B0B0B));
    g.FillRectangle(&dim, 0, 0, width_, height_);
    g.ResetClip();

    // Spotlight frame glides from step to step.
    if (has_spot) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_ms_) * 0.005f);
        Gdiplus::RectF outer = spot;
        outer.Inflate(3.0f + 2.0f * pulse, 3.0f + 2.0f * pulse);
        draw_round_rect(g, outer, 15, with_alpha(kAccent, 0.18f * (1.0f - pulse)), 1.0f);
        draw_round_rect(g, spot, 12, kAccent, 2.0f);
        anim_busy_ = true;
    }

    std::wstring title;
    std::wstring body;
    std::wstring next_label = tr(L"Далее", L"Next");
    switch (tour_step_) {
    case TourStep::Welcome:
        title = tr(L"Добро пожаловать в UniyaOver", L"Welcome to UniyaOver");
        body = tr(
            L"Спасибо, что пользуетесь нашей программой!\n\n"
            L"Нужна NVIDIA с NVENC (AMD/Intel пока нет).\n"
            L"Не для соревновательного режима с античитом\n"
            L"(Valorant и др.) — риск кика/бана на вашей стороне.\n\n"
            L"Дальше — короткий гайд меньше минуты.",
            L"Thanks for using our app!\n\n"
            L"Requires an NVIDIA GPU with NVENC (no AMD/Intel yet).\n"
            L"Not meant for competitive games with anti-cheat\n"
            L"(Valorant etc.) — any kick/ban risk is on you.\n\n"
            L"Next up: a short guide, under a minute.");
        next_label = tr(L"Пройти обучение", L"Start the tour");
        break;
    case TourStep::Idea:
        title = tr(L"Как это работает", L"How it works");
        body = tr(
            L"UniyaOver не пишет всё подряд. Сначала запускается\n"
            L"кольцевой буфер последних секунд экрана.\n\n"
            L"Важно: если буфер не запущен (IDLE), сохранить клип нельзя.",
            L"UniyaOver doesn't record everything. First you start\n"
            L"a ring buffer that keeps the last seconds of your screen.\n\n"
            L"Important: if the buffer is off (IDLE), there's nothing to save.");
        break;
    case TourStep::Hotkeys:
        title = tr(L"Горячие клавиши", L"Hotkeys");
        body = tr(
            L"1) Старт буфера — пишем экран в RAM\n"
            L"2) Сохранить весь экран — полный откат\n"
            L"3) Область — ножницы + откат только зоны\n"
            L"4) Стоп — выключает буфер\n\n"
            L"Сочетания меняются в Настройках → Хоткеи.",
            L"1) Start buffer — records the screen into RAM\n"
            L"2) Save full screen — the whole replay\n"
            L"3) Region — snip an area, replay only that part\n"
            L"4) Stop — turns the buffer off\n\n"
            L"Change them in Settings → Hotkeys.");
        break;
    case TourStep::Clips:
        title = tr(L"Где клипы", L"Where clips go");
        body = tr(
            L"Справа — последние записи. Клик открывает файл.\n"
            L"«Все ›» и раздел Библиотека — полный список.\n\n"
            L"Папку сохранения можно выбрать в Настройках → Клипы.",
            L"Recent clips are on the right. Click one to open it.\n"
            L"“All ›” and the Library page show the full list.\n\n"
            L"Pick the save folder in Settings → Clips.");
        break;
    case TourStep::Nav:
        title = tr(L"Навигация", L"Navigation");
        body = tr(
            L"Главная — обзор и хоткеи.\n"
            L"Библиотека — все клипы.\n"
            L"Настройки — качество, устройства, папка.\n\n"
            L"Внизу слева статус: IDLE или BUFFERING.",
            L"Home — overview and hotkeys.\n"
            L"Library — all your clips.\n"
            L"Settings — quality, devices, folder.\n\n"
            L"Bottom left shows the status: IDLE or BUFFERING.");
        break;
    case TourStep::Settings:
        title = tr(L"Настройки", L"Settings");
        body = tr(
            L"Качество: кодек (Авто/H.264/HEVC), FPS, битрейт, буфер.\n"
            L"Устройства: монитор и звук.\n"
            L"Клипы: папка сохранения. Уведомление: вид и место.\n\n"
            L"Только NVIDIA + NVENC. Без этого запись не стартует.",
            L"Quality: codec (Auto/H.264/HEVC), FPS, bitrate, buffer.\n"
            L"Devices: monitor and audio.\n"
            L"Clips: save folder. Notification: look and position.\n\n"
            L"NVIDIA + NVENC only. Recording won't start without it.");
        break;
    case TourStep::Finish:
        title = tr(L"Готово — можно пользоваться", L"All set — enjoy");
        body = tr(
            L"Чеклист:\n"
            L"• Старт буфера → статус BUFFERING\n"
            L"• Момент → Сохранить / Область\n"
            L"• Клип справа или в Библиотеке\n\n"
            L"Клипы только на этом ПК. Античит — на свой риск.\n"
            L"Гайд снова: «Обучение ›» слева.",
            L"Checklist:\n"
            L"• Start the buffer → status BUFFERING\n"
            L"• Great moment → Save / Region\n"
            L"• Find the clip on the right or in Library\n\n"
            L"Clips stay on this PC. Anti-cheat games at your own risk.\n"
            L"Replay the guide: “Tutorial ›” on the left.");
        next_label = tr(L"Начать", L"Let's go");
        break;
    default:
        return;
    }

    const float card_w = 520.0f;
    const float card_h = 280.0f;
    const float card_x = (static_cast<float>(width_) - card_w) * 0.5f;
    float card_target_y = (static_cast<float>(height_) - card_h) * 0.5f;

    // Keep card away from spotlight when possible
    if (tour_spotlight_.Width > 1.0f) {
        const float spot_mid_y = tour_spotlight_.Y + tour_spotlight_.Height * 0.5f;
        if (spot_mid_y > static_cast<float>(height_) * 0.55f) {
            card_target_y = 48.0f;
        } else if (spot_mid_y < static_cast<float>(height_) * 0.45f) {
            card_target_y = static_cast<float>(height_) - card_h - 40.0f;
        }
    }
    const float card_y = smooth(anim_key("tour.card.y"), card_target_y, 12.0f, 0.3f);

    Gdiplus::RectF card(card_x, card_y, card_w, card_h);
    fill_round_rect(g, card, 16, kCard);
    draw_round_rect(g, card, 16, kBorder, 1.0f);

    // Step content fades and rises in.
    const float a = ease_out(since / 380.0f);
    if (a < 1.0f) {
        anim_busy_ = true;
    }
    const float rise = (1.0f - a) * 10.0f;

    const int idx = onboarding_step_index();
    const int total = onboarding_step_count();
    draw_text(
        g,
        tr(L"Шаг ", L"Step ") + std::to_wstring(idx) + L" / " + std::to_wstring(total),
        Gdiplus::RectF(card.X + 24, card.Y + 18, card.Width - 48, 18),
        kTextMuted,
        9.0f,
        false);
    // Progress dots.
    for (int i = 1; i <= total; ++i) {
        const float on = smooth(anim_key("tour.dot", i), i <= idx ? 1.0f : 0.0f, 12.0f);
        const float w = 6.0f + 10.0f * smooth(anim_key("tour.dot.w", i), i == idx ? 1.0f : 0.0f, 12.0f);
        const float x = card.X + card.Width - 24 - static_cast<float>(total - i) * 20.0f - w;
        fill_round_rect(g, Gdiplus::RectF(x, card.Y + 23, w, 6), 3, mix_argb(kBorder, kAccent, on));
    }
    draw_text(
        g,
        title,
        Gdiplus::RectF(card.X + 24, card.Y + 42 + rise, card.Width - 48, 28),
        with_alpha(kText, a),
        15.0f,
        true);
    draw_text(
        g,
        body,
        Gdiplus::RectF(card.X + 24, card.Y + 78 + rise, card.Width - 48, 140),
        with_alpha(kTextDim, a),
        10.5f,
        false);

    const Gdiplus::RectF skip(card.X + 24, card.Y + card.Height - 52, 120, 34);
    const Gdiplus::RectF next(card.X + card.Width - 168, card.Y + card.Height - 52, 144, 34);
    const float skip_h = hover_amount(skip);
    const float next_h = hover_amount(next);
    fill_round_rect(g, skip, 10, mix_argb(kNavActive, kCardHover, skip_h));
    fill_round_rect(g, next, 10, mix_argb(kAccent, 0xFFFFFFFF, next_h * 0.6f));
    draw_text(
        g,
        tr(L"Пропустить", L"Skip"),
        skip,
        mix_argb(kTextDim, kText, skip_h),
        10.0f,
        false,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
    draw_text(
        g,
        next_label + L"  ›",
        Gdiplus::RectF(next.X + 2.0f * next_h, next.Y, next.Width, next.Height),
        kInk,
        10.5f,
        true,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
    tour_skip_hit_ = to_rect(skip);
    tour_next_hit_ = to_rect(next);
}

}  // namespace ir::gui
