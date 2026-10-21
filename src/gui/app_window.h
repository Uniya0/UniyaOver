#pragma once

#include "capture/capture_session.h"
#include "gui/clip_list.h"
#include "gui/clip_thumbnail.h"
#include "gui/gdi_utils.h"
#include "gui/save_toast.h"
#include "util/capability_probe.h"
#include "util/devices.h"
#include "util/i18n.h"
#include "util/settings.h"

#include <Shellapi.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ir::gui {

class AppWindow {
public:
    AppWindow() = default;
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool create(
        HINSTANCE instance,
        ir::capture::CaptureSession* capture,
        ir::settings::AppSettings settings,
        ir::hw::HardwareProfile hardware);

    int run_message_loop();

    HWND hwnd() const { return hwnd_; }

private:
    enum class Page { Home, Library, Settings };
    enum class SettingsTab { Quality, Hotkeys, Devices, Clips, Notify };
    enum class ListenTarget { None, Arm, Save, Stop, Region };
    enum class TourStep {
        None = 0,
        Welcome,
        Idea,
        Hotkeys,
        Clips,
        Nav,
        Settings,
        Finish,
    };

    static LRESULT CALLBACK static_wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT wnd_proc(UINT msg, WPARAM w_param, LPARAM l_param);

    void on_paint();
    void on_click(int x, int y);
    void on_rclick(int x, int y);
    void begin_hotkey_listen(ListenTarget target);
    void cancel_hotkey_listen();
    void on_listen_keydown(WPARAM vk);
    void on_listen_keyup(WPARAM vk);
    bool commit_hotkey(unsigned vk, unsigned modifiers);
    void begin_region_save();
    void request_full_save();
    void on_clip_saved(WPARAM flags, LPARAM deciseconds);
    void show_idle_toast();
    void apply_toast_style();
    void show_toast_preview();
    void refresh_clips();
    void refresh_device_lists();
    void apply_and_save();
    void rebind_hotkeys();
    void sync_clips_path();
    bool browse_clips_folder();
    void show_clip_context_menu(int index, int x, int y);

    void start_thumbnail_worker();
    void stop_thumbnail_worker();
    void request_thumbnails_for_clips();
    void thumbnail_worker_main();
    void ingest_ready_thumbnails();
    Gdiplus::Bitmap* thumbnail_for(const std::wstring& path);

    void tray_add();
    void tray_remove();
    void tray_show_menu();
    void minimize_to_tray();
    void restore_from_tray();

    void paint_sidebar(Gdiplus::Graphics& g);
    void paint_home(Gdiplus::Graphics& g);
    void paint_library(Gdiplus::Graphics& g);
    void paint_settings(Gdiplus::Graphics& g);
    void paint_clips_panel(Gdiplus::Graphics& g, const Gdiplus::RectF& area);
    void paint_clip_rows(
        Gdiplus::Graphics& g, const Gdiplus::RectF& list_area, int max_rows, uint32_t bg);
    void paint_buffer_meter(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float appear);
    void paint_onboarding(Gdiplus::Graphics& g);

    // Animation helpers (valid only inside on_paint).
    // Eases the value stored under `key` toward `target`; first use snaps to target.
    float smooth(uint32_t key, float target, float speed = 14.0f, float eps = 0.004f);
    // 0..1 eased hover amount; also marks `r` as clickable (hand cursor).
    float hover_amount(const Gdiplus::RectF& r);
    // Eased 0..1 progress of the current page/tab entrance.
    float page_progress(float delay_ms, float duration_ms);
    void update_anim_timer();
    void on_mouse_move(int x, int y);
    void set_language(ir::i18n::Lang lang);
    void flash_hotkey(int index);

    void start_onboarding();
    void finish_onboarding(bool mark_done);
    void onboarding_next();
    void prepare_onboarding_step();
    bool handle_onboarding_click(int x, int y);
    int onboarding_step_count() const;
    int onboarding_step_index() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ir::capture::CaptureSession* capture_ = nullptr;
    ir::settings::AppSettings settings_{};
    ir::hw::HardwareProfile hw_{};

    Page page_ = Page::Home;
    SettingsTab settings_tab_ = SettingsTab::Quality;
    ListenTarget listen_ = ListenTarget::None;
    TourStep tour_step_ = TourStep::None;
    unsigned listen_mods_ = 0;  // only keys pressed AFTER listen started
    int width_ = 1100;
    int height_ = 720;
    bool in_tray_ = false;
    NOTIFYICONDATAW tray_{};
    SaveToast save_toast_;

    RECT nav_home_{};
    RECT nav_lib_{};
    RECT nav_set_{};
    RECT tab_quality_{};
    RECT tab_hotkeys_{};
    RECT tab_devices_{};
    RECT tab_clips_{};
    RECT tab_notify_{};
    RECT lang_ru_hit_{};
    RECT lang_en_hit_{};
    RECT home_hk_hit_[4]{};
    RECT toast_on_hit_{};
    RECT toast_off_hit_{};
    RECT toast_size_hit_[3]{};
    RECT toast_pos_hit_[6]{};
    RECT toast_preview_hit_{};
    RECT codec_auto_{};
    RECT codec_h264_{};
    RECT codec_hevc_{};
    RECT fps60_{};
    RECT fps144_{};
    RECT fps240_{};
    RECT bitrate_minus_{};
    RECT bitrate_plus_{};
    RECT buffer_minus_{};
    RECT buffer_plus_{};
    RECT path_hit_{};
    RECT all_clips_hit_{};
    RECT telegram_hit_{};
    RECT hk_arm_hit_{};
    RECT hk_save_hit_{};
    RECT hk_stop_hit_{};
    RECT hk_region_hit_{};
    RECT monitor_hit_{};
    RECT audio_hit_{};
    RECT clips_browse_hit_{};
    RECT clips_reset_hit_{};
    RECT tour_next_hit_{};
    RECT tour_skip_hit_{};
    RECT tour_help_hit_{};
    Gdiplus::RectF tour_spotlight_{};

    struct ClipHit {
        RECT row{};
        RECT menu{};
        int index = -1;
    };
    std::vector<ClipHit> clip_hits_;

    std::vector<ClipInfo> clips_;
    std::wstring free_space_;
    std::vector<ir::devices::MonitorInfo> monitors_;
    std::vector<ir::devices::AudioDeviceInfo> audio_devices_;

    std::unordered_map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> thumb_cache_;
    std::unordered_map<std::wstring, ThumbnailPixels> thumb_ready_pixels_;
    std::unordered_set<std::wstring> thumb_inflight_;
    std::unordered_set<std::wstring> thumb_failed_;
    std::queue<std::wstring> thumb_queue_;
    std::mutex thumb_mutex_;
    std::condition_variable thumb_cv_;
    std::thread thumb_thread_;
    std::atomic<bool> thumb_stop_{false};
    int clip_refresh_ticks_ = 0;

    struct AnimValue {
        float value = 0.0f;
        uint32_t seen = 0;
    };
    std::unordered_map<uint32_t, AnimValue> anim_;
    uint32_t frame_no_ = 0;
    ULONGLONG now_ms_ = 0;
    ULONGLONG frame_ms_ = 0;
    float frame_dt_ = 0.0f;
    bool anim_busy_ = false;
    bool anim_timer_on_ = false;
    bool painting_tour_ = false;
    Page painted_page_ = Page::Home;
    SettingsTab painted_tab_ = SettingsTab::Quality;
    TourStep painted_tour_ = TourStep::None;
    ULONGLONG page_enter_ms_ = 0;
    ULONGLONG tour_enter_ms_ = 0;
    ULONGLONG armed_since_ms_ = 0;
    ULONGLONG new_clip_ms_ = 0;
    ULONGLONG flash_ms_[4]{};
    std::wstring newest_clip_;
    bool clips_scanned_ = false;
    POINT mouse_{-10000, -10000};
    bool mouse_tracking_ = false;
    int hot_index_ = -1;
    std::vector<RECT> hot_rects_;

    ULONG_PTR gdiplus_token_ = 0;
};

}  // namespace ir::gui
