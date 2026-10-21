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

constexpr UINT_PTR kTimerStatus = 1;
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

float toast_size_factor(int size) {
    constexpr float kFactors[] = {0.8f, 1.0f, 1.3f};
    return kFactors[(std::max)(0, (std::min)(2, size))];
}

std::wstring clip_length_label(double seconds) {
    const int total = (std::max)(1, static_cast<int>(std::lround(seconds)));
    if (total < 60) {
        return std::to_wstring(total) + L" с";
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
            if (capture_) {
                capture_->arm();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        },
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
            request_full_save();
            InvalidateRect(hwnd_, nullptr, FALSE);
        },
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
            if (capture_) {
                capture_->stop();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        },
        [this] {
            if (tour_step_ != TourStep::None) {
                return;
            }
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
    dialog->SetTitle(L"Папка для клипов UniyaOver");
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
    AppendMenuW(menu, MF_STRING, kTrayOpen, L"Открыть UniyaOver");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayArm, L"Старт буфера");
    AppendMenuW(menu, MF_STRING, kTraySave, L"Сохранить весь экран");
    AppendMenuW(menu, MF_STRING, kTrayRegion, L"Сохранить область");
    AppendMenuW(menu, MF_STRING, kTrayStop, L"Стоп буфера");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Выход");
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
        case WM_TIMER:
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
    free_space_ = free_space_display(clips_dir_wide());
    request_thumbnails_for_clips();
}

void AppWindow::show_clip_context_menu(int index, int x, int y) {
    if (index < 0 || index >= static_cast<int>(clips_.size())) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Открыть");
    AppendMenuW(menu, MF_STRING, 2, L"Показать в проводнике");
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
        L"Сохранено",
        L"так будет выглядеть уведомление",
        MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
}

void AppWindow::show_idle_toast() {
    if (!settings_.toast_enabled) {
        return;
    }
    save_toast_.show(
        SaveToast::Kind::Warning,
        L"Буфер не запущен",
        utf8(ir::settings::hotkey_label(settings_.hk_arm)) + L" — старт",
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
        const std::wstring what = region ? L"область" : L"весь экран";
        save_toast_.show(
            SaveToast::Kind::Saved,
            L"Сохранено",
            what + L" · " + clip_length_label(static_cast<double>(deciseconds) / 10.0),
            monitor);
    } else if (!region && deciseconds == 0) {
        save_toast_.show(
            SaveToast::Kind::Warning, L"Буфер ещё пуст", L"подождите пару секунд", monitor);
    } else {
        save_toast_.show(
            SaveToast::Kind::Error, L"Не удалось сохранить", L"подробности в логе", monitor);
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

void AppWindow::on_paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = rc.right - rc.left;
    height_ = rc.bottom - rc.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width_, height_);
    HGDIOBJ old = SelectObject(mem, bmp);

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
    toast_preview_hit_ = {};
    tour_next_hit_ = {};
    tour_skip_hit_ = {};
    tour_help_hit_ = {};
    tour_spotlight_ = {};

    paint_sidebar(g);
    if (page_ == Page::Home) {
        paint_home(g);
    } else if (page_ == Page::Library) {
        paint_library(g);
    } else {
        paint_settings(g);
    }
    if (tour_step_ != TourStep::None) {
        paint_onboarding(g);
    }

    BitBlt(hdc, 0, 0, width_, height_, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

void AppWindow::paint_sidebar(Gdiplus::Graphics& g) {
    using namespace theme;
    const float sw = static_cast<float>(kSidebarW);
    fill_round_rect(g, Gdiplus::RectF(0, 0, sw, static_cast<float>(height_)), 0, kSidebar);

    draw_brand_u(g, Gdiplus::RectF(18, 20, 32, 32), kText);
    draw_text(g, L"UniyaOver", Gdiplus::RectF(56, 18, 130, 26), kText, 11.0f, true);
    draw_text(g, L"v1.1.0", Gdiplus::RectF(56, 42, 130, 18), kTextMuted, 9.0f, false);

    const float nav_y0 = 90.0f;
    auto paint_nav = [&](float y, const wchar_t* label, bool active, RECT& hit) {
        Gdiplus::RectF r(12, y, sw - 24, static_cast<float>(kNavH));
        if (active) {
            fill_round_rect(g, r, 10, kNavActive);
        }
        draw_text(
            g,
            label,
            Gdiplus::RectF(r.X + 16, r.Y, r.Width - 20, r.Height),
            active ? kText : kTextDim,
            11.0f,
            active,
            Gdiplus::StringAlignmentNear,
            Gdiplus::StringAlignmentCenter);
        hit = to_rect(r);
    };
    paint_nav(nav_y0, L"Главная", page_ == Page::Home, nav_home_);
    paint_nav(nav_y0 + 48, L"Библиотека", page_ == Page::Library, nav_lib_);
    paint_nav(nav_y0 + 96, L"Настройки", page_ == Page::Settings, nav_set_);

    const bool armed = capture_ && capture_->is_armed();
    draw_text(
        g,
        armed ? L"● BUFFERING" : L"○ IDLE",
        Gdiplus::RectF(20, static_cast<float>(height_) - 110, sw - 40, 20),
        armed ? kStatusLive : kStatusIdle,
        9.5f,
        false);
    Gdiplus::RectF help(20, static_cast<float>(height_) - 88, sw - 40, 18);
    draw_text(g, L"Обучение ›", help, kTextDim, 9.0f, false);
    tour_help_hit_ = to_rect(help);
    draw_text(
        g,
        L"© UNIYA",
        Gdiplus::RectF(20, static_cast<float>(height_) - 64, sw - 40, 18),
        kText,
        10.0f,
        true);
    Gdiplus::RectF tg(20, static_cast<float>(height_) - 42, sw - 40, 20);
    draw_text(g, L"t.me/UNIYA0", tg, kTextDim, 9.0f, false);
    telegram_hit_ = to_rect(tg);
}

void AppWindow::paint_clips_panel(Gdiplus::Graphics& g, const Gdiplus::RectF& area) {
    using namespace theme;
    fill_round_rect(g, area, 0, kPanel);
    draw_text(
        g, L"Последние записи", Gdiplus::RectF(area.X + 18, area.Y + 16, 180, 24), kText, 12.0f, true);
    Gdiplus::RectF all_r(area.X + area.Width - 70, area.Y + 16, 55, 24);
    draw_text(g, L"Все >", all_r, kTextDim, 10.0f, false, Gdiplus::StringAlignmentFar);
    all_clips_hit_ = to_rect(all_r);

    paint_clip_rows(
        g, Gdiplus::RectF(area.X + 12, area.Y + 52, area.Width - 24, area.Height - 110), 6);

    const std::wstring dir = clips_dir_wide();
    Gdiplus::RectF path_r(area.X + 16, area.Y + area.Height - 48, area.Width - 32, 18);
    draw_text(g, dir, path_r, kTextMuted, 8.0f, false);
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
    Gdiplus::Graphics& g, const Gdiplus::RectF& list_area, int max_rows) {
    using namespace theme;
    if (clips_.empty()) {
        draw_text(
            g,
            L"Пока нет клипов.\nХоткей старта → буфер, хоткей сохранения → MP4.",
            list_area,
            kTextMuted,
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
        Gdiplus::RectF row(list_area.X, y, list_area.Width, static_cast<float>(kClipRowH));
        fill_round_rect(g, row, 10, kCard);
        Gdiplus::RectF thumb(row.X + 10, row.Y + 12, kThumbDrawW, kThumbDrawH);
        fill_round_rect(g, thumb, 8, kThumb);
        const auto& c = clips_[static_cast<size_t>(i)];
        if (Gdiplus::Bitmap* preview = thumbnail_for(c.path)) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetClip(thumb, Gdiplus::CombineModeReplace);
            g.DrawImage(preview, thumb);
            g.ResetClip();
        } else {
            draw_text(
                g,
                L"▶",
                thumb,
                kTextDim,
                14.0f,
                false,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
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
            kTextMuted,
            8.5f,
            false);
        Gdiplus::RectF menu(row.X + row.Width - 36, row.Y + 20, 28, 28);
        draw_text(
            g,
            L"⋯",
            menu,
            kTextDim,
            14.0f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
        ClipHit hit;
        hit.row = to_rect(row);
        hit.menu = to_rect(menu);
        hit.index = i;
        clip_hits_.push_back(hit);
    }
}

void AppWindow::paint_home(Gdiplus::Graphics& g) {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    const float right_w = static_cast<float>(kRightPanelW);
    const float mid_w = static_cast<float>(width_) - left - right_w;

    const float mark = 84.0f;
    draw_brand_uo_ring(
        g,
        Gdiplus::RectF(left + mid_w * 0.5f - mark * 0.5f, 36.0f, mark, mark),
        kText);
    draw_text(
        g,
        L"UniyaOver",
        Gdiplus::RectF(left + 40, 128, mid_w - 80, 36),
        kText,
        22.0f,
        true,
        Gdiplus::StringAlignmentCenter);

    const std::wstring desc =
        L"Простое приложение для записи последних " + std::to_wstring(settings_.buffer_sec) +
        L" секунд\nэкрана/игры по горячей клавише.";
    draw_text(
        g,
        desc,
        Gdiplus::RectF(left + 50, 168, mid_w - 100, 52),
        kTextDim,
        10.5f,
        false,
        Gdiplus::StringAlignmentCenter);

    const float feat_y = 236.0f;
    auto feature = [&](float y, const wchar_t* title, const wchar_t* sub) {
        draw_text(
            g,
            title,
            Gdiplus::RectF(left + 70, y, mid_w - 140, 22),
            kText,
            12.0f,
            true,
            Gdiplus::StringAlignmentCenter);
        draw_text(
            g,
            sub,
            Gdiplus::RectF(left + 70, y + 22, mid_w - 140, 20),
            kTextMuted,
            9.5f,
            false,
            Gdiplus::StringAlignmentCenter);
    };
    feature(feat_y, L"Мгновенная запись", L"Запускается по хоткею");
    feature(feat_y + 60, L"Кольцевой буфер", L"Сохраняет нужный момент");
    feature(feat_y + 120, L"Лёгкий и минималистичный", L"Ничего лишнего");

    Gdiplus::RectF card(left + 48, static_cast<float>(height_) - 236, mid_w - 96, 186);
    fill_round_rect(g, card, 14, kCard);
    draw_round_rect(g, card, 14, kBorder, 1.0f);
    draw_text(
        g,
        L"Горячие клавиши",
        Gdiplus::RectF(card.X + 20, card.Y + 14, card.Width - 40, 22),
        kText,
        11.0f,
        true,
        Gdiplus::StringAlignmentCenter);

    auto hk_pill = [&](float y, const std::wstring& keys, const wchar_t* action) {
        Gdiplus::RectF pill(card.X + 20, y, card.Width - 40, 28);
        fill_round_rect(g, pill, 14, kNavActive);
        draw_text(
            g,
            keys + L"   —   " + action,
            Gdiplus::RectF(pill.X + 12, pill.Y, pill.Width - 24, pill.Height),
            kText,
            9.5f,
            false,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
    };
    hk_pill(card.Y + 44, utf8(ir::settings::hotkey_label(settings_.hk_arm)), L"старт буфера");
    hk_pill(card.Y + 76, utf8(ir::settings::hotkey_label(settings_.hk_save)), L"весь экран");
    hk_pill(card.Y + 108, utf8(ir::settings::hotkey_label(settings_.hk_region)), L"область");
    hk_pill(card.Y + 140, utf8(ir::settings::hotkey_label(settings_.hk_stop)), L"стоп буфера");

    paint_clips_panel(g, Gdiplus::RectF(left + mid_w, 0, right_w, static_cast<float>(height_)));
}

void AppWindow::paint_library(Gdiplus::Graphics& g) {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    Gdiplus::RectF area(left, 0, static_cast<float>(width_) - left, static_cast<float>(height_));
    draw_text(g, L"Библиотека", Gdiplus::RectF(left + 28, 24, 300, 32), kText, 18.0f, true);
    const std::wstring dir = clips_dir_wide();
    draw_text(
        g,
        dir + L"   ·   " + free_space_,
        Gdiplus::RectF(left + 28, 58, area.Width - 56, 20),
        kTextMuted,
        9.0f,
        false);
    path_hit_ = to_rect(Gdiplus::RectF(left + 28, 58, area.Width - 56, 20));
    paint_clip_rows(g, Gdiplus::RectF(left + 24, 96, area.Width - 48, area.Height - 120), 12);
}

void AppWindow::paint_settings(Gdiplus::Graphics& g) {
    using namespace theme;
    const float left = static_cast<float>(kSidebarW);
    Gdiplus::RectF area(left, 0, static_cast<float>(width_) - left, static_cast<float>(height_));

    draw_text(g, L"Настройки", Gdiplus::RectF(left + 28, 18, 300, 32), kText, 18.0f, true);
    {
        std::wstring hw_line = ir::hw::status_line(hw_);
        if (hw_.capture_ready) {
            const auto resolved = ir::hw::resolve_codec(settings_, hw_);
            hw_line += L" · кодек ";
            hw_line += (resolved == ir::hw::VideoCodec::Hevc) ? L"HEVC" : L"H.264";
            if (settings_.video_codec == "auto") {
                hw_line += L" (авто)";
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
        L"Сохраняются на диск. FPS/устройства — со следующего старта буфера.",
        Gdiplus::RectF(left + 28, 74, area.Width - 56, 18),
        kTextMuted,
        9.0f,
        false);

    // Tabs
    float tab_y = 104.0f;
    auto tab_btn = [&](float x, float w, const wchar_t* label, bool on, RECT& hit) {
        Gdiplus::RectF r(x, tab_y, w, 34);
        fill_round_rect(g, r, 10, on ? kAccent : kCard);
        draw_text(
            g,
            label,
            r,
            on ? 0xFF0B0B0B : kTextDim,
            10.0f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
        hit = to_rect(r);
    };
    const float tab_gap = 8.0f;
    const float tab_w = 118.0f;
    float tx = left + 28;
    tab_btn(tx, tab_w, L"Качество", settings_tab_ == SettingsTab::Quality, tab_quality_);
    tx += tab_w + tab_gap;
    tab_btn(tx, tab_w, L"Хоткеи", settings_tab_ == SettingsTab::Hotkeys, tab_hotkeys_);
    tx += tab_w + tab_gap;
    tab_btn(tx, tab_w, L"Устройства", settings_tab_ == SettingsTab::Devices, tab_devices_);
    tx += tab_w + tab_gap;
    tab_btn(tx, tab_w, L"Клипы", settings_tab_ == SettingsTab::Clips, tab_clips_);
    tx += tab_w + tab_gap;
    tab_btn(tx, tab_w, L"Уведомление", settings_tab_ == SettingsTab::Notify, tab_notify_);

    float y = tab_y + 52.0f;

    auto choice_btn = [&](float x, float w, const wchar_t* label, bool on, RECT& hit) {
        Gdiplus::RectF r(x, y, w, 34);
        fill_round_rect(g, r, 10, on ? kAccent : kCard);
        draw_round_rect(g, r, 10, kBorder, 1.0f);
        draw_text(
            g,
            label,
            r,
            on ? 0xFF0B0B0B : kText,
            10.5f,
            true,
            Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter);
        hit = to_rect(r);
    };

    if (settings_tab_ == SettingsTab::Notify) {
        const bool enabled = settings_.toast_enabled;
        draw_text(g, L"Уведомление о сохранении", Gdiplus::RectF(left + 28, y, 400, 20), kText, 11.0f, true);
        y += 26;
        choice_btn(left + 28, 90, L"Вкл", enabled, toast_on_hit_);
        choice_btn(left + 126, 90, L"Выкл", !enabled, toast_off_hit_);

        y += 52;
        draw_text(g, L"Размер", Gdiplus::RectF(left + 28, y, 200, 20), enabled ? kText : kTextMuted, 11.0f, true);
        y += 26;
        const wchar_t* size_labels[] = {L"Маленький", L"Средний", L"Большой"};
        for (int i = 0; i < 3; ++i) {
            choice_btn(
                left + 28 + 128.0f * static_cast<float>(i),
                120,
                size_labels[i],
                settings_.toast_size == i,
                toast_size_hit_[i]);
        }

        y += 52;
        draw_text(
            g,
            L"Положение на экране",
            Gdiplus::RectF(left + 28, y, 300, 20),
            enabled ? kText : kTextMuted,
            11.0f,
            true);
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
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3;
            const int row = i / 3;
            const float px = col == 0 ? screen.X + inset
                           : col == 1 ? screen.X + (screen.Width - pill_w) * 0.5f
                                      : screen.GetRight() - inset - pill_w;
            const float py = row == 0 ? screen.Y + inset : screen.GetBottom() - inset - pill_h;
            const Gdiplus::RectF pill(px, py, pill_w, pill_h);
            const bool on = settings_.toast_position == i;
            if (on) {
                fill_round_rect(g, pill, pill_h * 0.5f, enabled ? kAccent : kTextMuted);
                const float cx = pill.X + 10.0f;
                const float cy = pill.Y + pill_h * 0.5f;
                Gdiplus::SolidBrush dot(argb(0xFF0B0B0B));
                g.FillEllipse(&dot, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
                Gdiplus::Pen check(argb(enabled ? kAccent : kTextMuted), 1.6f);
                check.SetStartCap(Gdiplus::LineCapRound);
                check.SetEndCap(Gdiplus::LineCapRound);
                check.SetLineJoin(Gdiplus::LineJoinRound);
                const Gdiplus::PointF mark[] = {{cx - 2.6f, cy + 0.2f}, {cx - 0.6f, cy + 2.2f}, {cx + 2.8f, cy - 2.0f}};
                g.DrawLines(&check, mark, 3);
                fill_round_rect(g, Gdiplus::RectF(pill.X + 21, cy - 4.0f, 32, 3), 1.5f, 0xFF0B0B0B);
                fill_round_rect(g, Gdiplus::RectF(pill.X + 21, cy + 1.5f, 22, 2.5f), 1.25f, 0xFF4A4A4A);
            } else {
                fill_round_rect(g, pill, pill_h * 0.5f, 0xFF222222);
                draw_round_rect(g, pill, pill_h * 0.5f, 0xFF3A3A3A, 1.0f);
            }
            Gdiplus::RectF hit = pill;
            hit.Inflate(6.0f, 6.0f);
            toast_pos_hit_[i] = to_rect(hit);
        }

        const float side_x = screen.GetRight() + 28;
        const float side_w = (std::max)(200.0f, (std::min)(340.0f, area.GetRight() - 28 - side_x));
        if (enabled) {
            Gdiplus::RectF preview(side_x, y, 190, 34);
            fill_round_rect(g, preview, 10, kAccent);
            draw_text(
                g,
                L"Показать пример",
                preview,
                0xFF0B0B0B,
                10.5f,
                true,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
            toast_preview_hit_ = to_rect(preview);
        }
        draw_text(
            g,
            enabled ? L"Пример появится на мониторе, где открыто это окно."
                    : L"Уведомления выключены — сохранение работает молча.",
            Gdiplus::RectF(side_x, y + 48, side_w, 40),
            kTextDim,
            9.5f,
            false);
        draw_text(
            g,
            L"В клипы уведомление не попадает. Поверх игр в эксклюзивном полноэкранном "
            L"режиме Windows его не покажет — выбирайте «оконный без рамки».",
            Gdiplus::RectF(side_x, y + 92, side_w, 64),
            kTextMuted,
            9.0f,
            false);
        return;
    }

    if (settings_tab_ == SettingsTab::Quality) {
        draw_text(g, L"Кодек", Gdiplus::RectF(left + 28, y, 200, 20), kText, 11.0f, true);
        y += 26;
        auto codec_btn = [&](float x, float w, const wchar_t* label, bool on, bool allowed, RECT& hit) {
            Gdiplus::RectF r(x, y, w, 34);
            fill_round_rect(g, r, 10, on ? kAccent : kCard);
            draw_round_rect(g, r, 10, allowed ? kBorder : 0xFF333333, 1.0f);
            draw_text(
                g,
                label,
                r,
                on ? 0xFF0B0B0B : (allowed ? kText : kTextMuted),
                10.5f,
                true,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
            hit = allowed ? to_rect(r) : RECT{};
        };
        const bool hevc_ok = ir::hw::hevc_available(hw_);
        codec_btn(left + 28, 90, L"Авто", settings_.video_codec == "auto", true, codec_auto_);
        codec_btn(left + 126, 90, L"H.264", settings_.video_codec == "h264", true, codec_h264_);
        codec_btn(left + 224, 90, L"HEVC", settings_.video_codec == "hevc", hevc_ok, codec_hevc_);
        if (!hevc_ok) {
            draw_text(
                g,
                L"HEVC NVENC недоступен на этом ПК",
                Gdiplus::RectF(left + 328, y + 8, 320, 20),
                kTextMuted,
                9.0f,
                false);
        }

        y += 52;
        draw_text(g, L"FPS", Gdiplus::RectF(left + 28, y, 200, 20), kText, 11.0f, true);
        y += 26;
        auto fps_btn = [&](float x, int fps, RECT& hit) {
            Gdiplus::RectF r(x, y, 80, 34);
            const bool allowed = ir::hw::fps_allowed(hw_, fps);
            const bool on = settings_.fps_cap == fps && allowed;
            fill_round_rect(g, r, 10, on ? kAccent : kCard);
            draw_round_rect(g, r, 10, allowed ? kBorder : 0xFF333333, 1.0f);
            draw_text(
                g,
                std::to_wstring(fps),
                r,
                on ? 0xFF0B0B0B : (allowed ? kText : kTextMuted),
                11.0f,
                true,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
            hit = allowed ? to_rect(r) : RECT{};
        };
        fps_btn(left + 28, 60, fps60_);
        fps_btn(left + 118, 144, fps144_);
        fps_btn(left + 208, 240, fps240_);
        if (!hw_.capture_ready) {
            draw_text(
                g,
                L"NVENC недоступен — FPS нельзя выбрать",
                Gdiplus::RectF(left + 28, y + 42, 520, 22),
                0xFFC07070,
                9.0f,
                false);
        } else if (hw_.nvenc.recommended_max_fps < 240) {
            draw_text(
                g,
                L"Рекомендуется до " + std::to_wstring(hw_.nvenc.recommended_max_fps) +
                    L" FPS для этой карты",
                Gdiplus::RectF(left + 28, y + 42, 520, 22),
                kTextMuted,
                9.0f,
                false);
        }

        y += 70;
        draw_text(g, L"Битрейт (Mbps)", Gdiplus::RectF(left + 28, y, 200, 20), kText, 11.0f, true);
        draw_text(g, L"Буфер (сек)", Gdiplus::RectF(left + 280, y, 200, 20), kText, 11.0f, true);
        y += 26;
        auto stepper = [&](float x, const std::wstring& val, RECT& minus, RECT& plus) {
            Gdiplus::RectF m(x, y, 36, 34);
            Gdiplus::RectF v(x + 42, y, 70, 34);
            Gdiplus::RectF p(x + 118, y, 36, 34);
            fill_round_rect(g, m, 10, kCard);
            fill_round_rect(g, v, 10, kCard);
            fill_round_rect(g, p, 10, kCard);
            draw_text(
                g, L"−", m, kText, 14.0f, true, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
            draw_text(
                g, val, v, kText, 12.0f, true, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
            draw_text(
                g, L"+", p, kText, 14.0f, true, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
            minus = to_rect(m);
            plus = to_rect(p);
        };
        stepper(left + 28, std::to_wstring(bitrate_mbps(settings_)), bitrate_minus_, bitrate_plus_);
        stepper(left + 280, std::to_wstring(settings_.buffer_sec), buffer_minus_, buffer_plus_);
        return;
    }

    if (settings_tab_ == SettingsTab::Hotkeys) {
        draw_text(
            g,
            L"Клик по полю, затем Ctrl+X или Ctrl+Shift+R… (Esc — отмена)",
            Gdiplus::RectF(left + 28, y, 620, 20),
            kTextMuted,
            9.5f,
            false);
        y += 28;
        auto hk_row = [&](float yy, const wchar_t* title, const ir::settings::HotkeyBinding& b, bool listening, RECT& hit) {
            draw_text(g, title, Gdiplus::RectF(left + 28, yy + 6, 120, 24), kTextDim, 10.0f, false);
            Gdiplus::RectF pill(left + 160, yy, 280, 34);
            fill_round_rect(g, pill, 10, listening ? kAccent : kCard);
            std::wstring label;
            if (listening) {
                if (listen_mods_ == 0) {
                    label = L"Жду сочетание…";
                } else {
                    ir::settings::HotkeyBinding preview;
                    preview.modifiers = listen_mods_;
                    preview.vk = 0;
                    label = utf8(ir::settings::hotkey_label(preview)) + L"…";
                }
            } else {
                label = utf8(ir::settings::hotkey_label(b));
            }
            draw_text(
                g,
                label,
                pill,
                listening ? 0xFF0B0B0B : kText,
                10.5f,
                true,
                Gdiplus::StringAlignmentCenter,
                Gdiplus::StringAlignmentCenter);
            hit = to_rect(pill);
        };
        hk_row(y, L"Старт", settings_.hk_arm, listen_ == ListenTarget::Arm, hk_arm_hit_);
        y += 42;
        hk_row(y, L"Весь экран", settings_.hk_save, listen_ == ListenTarget::Save, hk_save_hit_);
        y += 42;
        hk_row(y, L"Область", settings_.hk_region, listen_ == ListenTarget::Region, hk_region_hit_);
        y += 42;
        hk_row(y, L"Стоп", settings_.hk_stop, listen_ == ListenTarget::Stop, hk_stop_hit_);
        return;
    }

    if (settings_tab_ == SettingsTab::Devices) {
        draw_text(g, L"Экран (клик — следующий)", Gdiplus::RectF(left + 28, y, 400, 20), kText, 11.0f, true);
        y += 26;
        std::wstring mon_label = L"Основной монитор";
        if (settings_.monitor_index >= 0) {
            for (const auto& m : monitors_) {
                if (m.index == settings_.monitor_index) {
                    mon_label = m.label;
                    break;
                }
            }
            if (mon_label == L"Основной монитор") {
                mon_label = L"Монитор #" + std::to_wstring(settings_.monitor_index);
            }
        }
        Gdiplus::RectF mon(left + 28, y, (std::min)(area.Width - 56, 520.0f), 40);
        fill_round_rect(g, mon, 10, kCard);
        draw_text(
            g,
            mon_label,
            Gdiplus::RectF(mon.X + 14, mon.Y, mon.Width - 28, mon.Height),
            kText,
            10.5f,
            false,
            Gdiplus::StringAlignmentNear,
            Gdiplus::StringAlignmentCenter);
        monitor_hit_ = to_rect(mon);

        y += 58;
        draw_text(g, L"Звук (клик — следующее устройство)", Gdiplus::RectF(left + 28, y, 400, 20), kText, 11.0f, true);
        y += 26;
        std::wstring aud_label = L"Авто (активное воспроизведение)";
        if (!settings_.audio_device_id.empty()) {
            aud_label = L"Устройство не найдено";
            for (const auto& a : audio_devices_) {
                if (a.id == settings_.audio_device_id) {
                    aud_label = a.label;
                    break;
                }
            }
        }
        Gdiplus::RectF aud(left + 28, y, (std::min)(area.Width - 56, 520.0f), 40);
        fill_round_rect(g, aud, 10, kCard);
        draw_text(
            g,
            aud_label,
            Gdiplus::RectF(aud.X + 14, aud.Y, aud.Width - 28, aud.Height),
            kText,
            10.0f,
            false,
            Gdiplus::StringAlignmentNear,
            Gdiplus::StringAlignmentCenter);
        audio_hit_ = to_rect(aud);
        return;
    }

    // Clips tab
    draw_text(g, L"Папка клипов", Gdiplus::RectF(left + 28, y, 200, 20), kText, 11.0f, true);
    y += 26;
    const std::wstring clips_path = clips_dir_wide();
    Gdiplus::RectF path_card(left + 28, y, (std::min)(area.Width - 56, 520.0f), 40);
    fill_round_rect(g, path_card, 10, kCard);
    draw_text(
        g,
        clips_path,
        Gdiplus::RectF(path_card.X + 14, path_card.Y, path_card.Width - 28, path_card.Height),
        kTextDim,
        9.5f,
        false,
        Gdiplus::StringAlignmentNear,
        Gdiplus::StringAlignmentCenter);
    path_hit_ = to_rect(path_card);

    y += 52;
    Gdiplus::RectF browse(left + 28, y, 160, 34);
    Gdiplus::RectF reset(left + 200, y, 200, 34);
    fill_round_rect(g, browse, 10, kAccent);
    fill_round_rect(g, reset, 10, kCard);
    draw_text(
        g,
        L"Выбрать…",
        browse,
        0xFF0B0B0B,
        10.5f,
        true,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
    draw_text(
        g,
        L"Сбросить по умолчанию",
        reset,
        kText,
        10.0f,
        false,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
    clips_browse_hit_ = to_rect(browse);
    clips_reset_hit_ = to_rect(reset);
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
        tour_spotlight_ = Gdiplus::RectF(left + 40, 120, mid_w - 80, 110);
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

    // Dim overlay
    Gdiplus::SolidBrush dim(argb(0xCC0B0B0B));
    g.FillRectangle(&dim, 0, 0, width_, height_);

    // Spotlight hole: redraw a soft frame around focus area
    if (tour_spotlight_.Width > 1.0f && tour_spotlight_.Height > 1.0f) {
        draw_round_rect(g, tour_spotlight_, 12, kAccent, 2.0f);
    }

    std::wstring title;
    std::wstring body;
    std::wstring next_label = L"Далее";
    switch (tour_step_) {
    case TourStep::Welcome:
        title = L"Добро пожаловать в UniyaOver";
        body =
            L"Спасибо, что пользуетесь нашей программой!\n\n"
            L"Нужна NVIDIA с NVENC (AMD/Intel пока нет).\n"
            L"Не для соревновательного режима с античитом\n"
            L"(Valorant и др.) — риск кика/бана на вашей стороне.\n\n"
            L"Дальше — короткий гайд меньше минуты.";
        next_label = L"Пройти обучение";
        break;
    case TourStep::Idea:
        title = L"Как это работает";
        body =
            L"UniyaOver не пишет всё подряд. Сначала запускается\n"
            L"кольцевой буфер последних секунд экрана.\n\n"
            L"Важно: если буфер не запущен (IDLE), сохранить клип нельзя.";
        break;
    case TourStep::Hotkeys:
        title = L"Горячие клавиши";
        body =
            L"1) Старт буфера — пишем экран в RAM\n"
            L"2) Сохранить весь экран — полный откат\n"
            L"3) Область — ножницы + откат только зоны\n"
            L"4) Стоп — выключает буфер\n\n"
            L"Сочетания меняются в Настройках → Хоткеи.";
        break;
    case TourStep::Clips:
        title = L"Где клипы";
        body =
            L"Справа — последние записи. Клик открывает файл.\n"
            L"«Все ›» и раздел Библиотека — полный список.\n\n"
            L"Папку сохранения можно выбрать в Настройках → Клипы.";
        break;
    case TourStep::Nav:
        title = L"Навигация";
        body =
            L"Главная — обзор и хоткеи.\n"
            L"Библиотека — все клипы.\n"
            L"Настройки — качество, устройства, папка.\n\n"
            L"Внизу слева статус: IDLE или BUFFERING.";
        break;
    case TourStep::Settings:
        title = L"Настройки";
        body =
            L"Качество: кодек (Авто/H.264/HEVC), FPS, битрейт, буфер.\n"
            L"Устройства: монитор и звук.\n"
            L"Клипы: папка сохранения.\n\n"
            L"Только NVIDIA + NVENC. Без этого запись не стартует.";
        break;
    case TourStep::Finish:
        title = L"Готово — можно пользоваться";
        body =
            L"Чеклист:\n"
            L"• Старт буфера → статус BUFFERING\n"
            L"• Момент → Сохранить / Область\n"
            L"• Клип справа или в Библиотеке\n\n"
            L"Клипы только на этом ПК. Античит — на свой риск.\n"
            L"Гайд снова: «Обучение ›» слева.";
        next_label = L"Начать";
        break;
    default:
        return;
    }

    const float card_w = 520.0f;
    const float card_h = 280.0f;
    float card_x = (static_cast<float>(width_) - card_w) * 0.5f;
    float card_y = (static_cast<float>(height_) - card_h) * 0.5f;

    // Keep card away from spotlight when possible
    if (tour_spotlight_.Width > 1.0f) {
        const float spot_mid_y = tour_spotlight_.Y + tour_spotlight_.Height * 0.5f;
        if (spot_mid_y > static_cast<float>(height_) * 0.55f) {
            card_y = 48.0f;
        } else if (spot_mid_y < static_cast<float>(height_) * 0.45f) {
            card_y = static_cast<float>(height_) - card_h - 40.0f;
        }
    }

    Gdiplus::RectF card(card_x, card_y, card_w, card_h);
    fill_round_rect(g, card, 16, kCard);
    draw_round_rect(g, card, 16, kBorder, 1.0f);

    const int idx = onboarding_step_index();
    const int total = onboarding_step_count();
    draw_text(
        g,
        L"Шаг " + std::to_wstring(idx) + L" / " + std::to_wstring(total),
        Gdiplus::RectF(card.X + 24, card.Y + 18, card.Width - 48, 18),
        kTextMuted,
        9.0f,
        false);
    draw_text(
        g,
        title,
        Gdiplus::RectF(card.X + 24, card.Y + 42, card.Width - 48, 28),
        kText,
        15.0f,
        true);
    draw_text(
        g,
        body,
        Gdiplus::RectF(card.X + 24, card.Y + 78, card.Width - 48, 140),
        kTextDim,
        10.5f,
        false);

    Gdiplus::RectF skip(card.X + 24, card.Y + card.Height - 52, 120, 34);
    Gdiplus::RectF next(card.X + card.Width - 168, card.Y + card.Height - 52, 144, 34);
    fill_round_rect(g, skip, 10, kNavActive);
    fill_round_rect(g, next, 10, kAccent);
    draw_text(
        g,
        L"Пропустить",
        skip,
        kTextDim,
        10.0f,
        false,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
    draw_text(
        g,
        next_label,
        next,
        0xFF0B0B0B,
        10.5f,
        true,
        Gdiplus::StringAlignmentCenter,
        Gdiplus::StringAlignmentCenter);
    tour_skip_hit_ = to_rect(skip);
    tour_next_hit_ = to_rect(next);
}

}  // namespace ir::gui
