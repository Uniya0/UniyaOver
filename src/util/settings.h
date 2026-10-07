#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <string>

namespace ir::settings {

struct HotkeyBinding {
    // Win32 MOD_* flags without MOD_NOREPEAT (added at register time).
    unsigned modifiers = MOD_CONTROL | MOD_SHIFT;
    unsigned vk = 0;
};

struct AppSettings {
    int fps_cap = 144;       // 60 | 144 | 240
    int buffer_sec = 30;
    int video_bitrate = 0;   // 0 = auto from fps_cap
    // "auto" | "h264" | "hevc"
    std::string video_codec = "auto";

    HotkeyBinding hk_arm{MOD_CONTROL | MOD_SHIFT, 'R'};
    HotkeyBinding hk_save{MOD_CONTROL | MOD_SHIFT, 'X'};
    HotkeyBinding hk_stop{MOD_CONTROL | MOD_SHIFT, 'S'};
    // Region instant-replay (snipping crop of the same buffer).
    HotkeyBinding hk_region{MOD_CONTROL | MOD_SHIFT, 'A'};

    // -1 = primary monitor; otherwise 0-based index across attached outputs.
    int monitor_index = -1;
    // Empty = auto-pick playing device; non-empty = preferred WASAPI endpoint id.
    std::string audio_device_id;
    // Empty = default Videos\instant-replay.
    std::string clips_folder;

    // false = show interactive guide on next launch.
    bool onboarding_done = false;
};

// Defaults → config.json → CLI overrides.
AppSettings load(int argc, char** argv);
bool save(const AppSettings& s);

AppSettings from_args(int argc, char** argv);

int auto_bitrate(int fps_cap);
int auto_gop_frames(int fps_cap);
const char* nvenc_preset_for_fps(int fps_cap);

std::string describe(const AppSettings& s);
std::string hotkey_label(const HotkeyBinding& b);

}  // namespace ir::settings
