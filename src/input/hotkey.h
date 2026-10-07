#pragma once

#include "util/settings.h"

#include <functional>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace ir::hotkey {

inline constexpr unsigned int kWmArmBuffer = 0x0400 + 32;
inline constexpr unsigned int kWmSaveClip = 0x0400 + 33;
inline constexpr unsigned int kWmStopBuffer = 0x0400 + 34;
inline constexpr unsigned int kWmRegionSave = 0x0400 + 35;

struct Callbacks {
    std::function<void()> on_arm_buffer;
    std::function<void()> on_save_clip;
    std::function<void()> on_stop_buffer;
    std::function<void()> on_region_save;
};

struct Bindings {
    ir::settings::HotkeyBinding arm;
    ir::settings::HotkeyBinding save;
    ir::settings::HotkeyBinding stop;
    ir::settings::HotkeyBinding region;
};

bool install(HWND hwnd, const Callbacks& callbacks, const Bindings& bindings);
bool reinstall(const Bindings& bindings);
void uninstall(HWND hwnd);
void suspend();

bool handle_message(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

void request_quit();

}  // namespace ir::hotkey
