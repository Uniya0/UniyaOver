#include "input/hotkey.h"

#include "util/log.h"

#include <string>

namespace ir::hotkey {
namespace {

constexpr int kHotkeyArmId = 1;
constexpr int kHotkeySaveId = 2;
constexpr int kHotkeyStopId = 3;
constexpr int kHotkeyRegionId = 4;

HWND g_hwnd = nullptr;
Callbacks g_callbacks{};
Bindings g_bindings{};

bool register_one(int id, const ir::settings::HotkeyBinding& b, const char* name) {
    if (b.vk == 0) {
        ir::log::error(std::string("Hotkey ") + name + " has empty VK");
        return false;
    }
    const UINT mods = (b.modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) | MOD_NOREPEAT;
    if (!RegisterHotKey(g_hwnd, id, mods, b.vk)) {
        ir::log::error(
            std::string("RegisterHotKey(") + name + "=" + ir::settings::hotkey_label(b) +
            ") failed, error=" + std::to_string(GetLastError()));
        return false;
    }
    return true;
}

void unregister_all() {
    if (!g_hwnd) {
        return;
    }
    UnregisterHotKey(g_hwnd, kHotkeyArmId);
    UnregisterHotKey(g_hwnd, kHotkeySaveId);
    UnregisterHotKey(g_hwnd, kHotkeyStopId);
    UnregisterHotKey(g_hwnd, kHotkeyRegionId);
}

bool register_all(const Bindings& bindings) {
    unregister_all();
    g_bindings = bindings;
    if (!register_one(kHotkeyArmId, bindings.arm, "arm") ||
        !register_one(kHotkeySaveId, bindings.save, "save") ||
        !register_one(kHotkeyStopId, bindings.stop, "stop") ||
        !register_one(kHotkeyRegionId, bindings.region, "region")) {
        unregister_all();
        return false;
    }
    ir::log::info("Hotkeys:");
    ir::log::info("  " + ir::settings::hotkey_label(bindings.arm) + "  start buffering");
    ir::log::info("  " + ir::settings::hotkey_label(bindings.save) + "  save full clip");
    ir::log::info("  " + ir::settings::hotkey_label(bindings.region) + "  save region clip");
    ir::log::info("  " + ir::settings::hotkey_label(bindings.stop) + "  stop buffering");
    return true;
}

}  // namespace

bool install(HWND hwnd, const Callbacks& callbacks, const Bindings& bindings) {
    if (!hwnd) {
        return false;
    }
    uninstall(hwnd);
    g_hwnd = hwnd;
    g_callbacks = callbacks;
    if (!register_all(bindings)) {
        g_hwnd = nullptr;
        g_callbacks = {};
        return false;
    }
    return true;
}

bool reinstall(const Bindings& bindings) {
    if (!g_hwnd) {
        return false;
    }
    return register_all(bindings);
}

void suspend() {
    unregister_all();
}

void uninstall(HWND hwnd) {
    if (!g_hwnd) {
        return;
    }
    if (hwnd && hwnd != g_hwnd) {
        return;
    }
    unregister_all();
    g_hwnd = nullptr;
    g_callbacks = {};
    g_bindings = {};
}

bool handle_message(HWND hwnd, UINT msg, WPARAM w_param, LPARAM /*l_param*/) {
    if (hwnd != g_hwnd) {
        return false;
    }

    if (msg == WM_HOTKEY) {
        if (static_cast<int>(w_param) == kHotkeyArmId) {
            PostMessageW(hwnd, kWmArmBuffer, 0, 0);
            return true;
        }
        if (static_cast<int>(w_param) == kHotkeySaveId) {
            PostMessageW(hwnd, kWmSaveClip, 0, 0);
            return true;
        }
        if (static_cast<int>(w_param) == kHotkeyStopId) {
            PostMessageW(hwnd, kWmStopBuffer, 0, 0);
            return true;
        }
        if (static_cast<int>(w_param) == kHotkeyRegionId) {
            PostMessageW(hwnd, kWmRegionSave, 0, 0);
            return true;
        }
        return false;
    }

    if (msg == kWmArmBuffer) {
        if (g_callbacks.on_arm_buffer) {
            g_callbacks.on_arm_buffer();
        }
        return true;
    }
    if (msg == kWmSaveClip) {
        if (g_callbacks.on_save_clip) {
            g_callbacks.on_save_clip();
        }
        return true;
    }
    if (msg == kWmStopBuffer) {
        if (g_callbacks.on_stop_buffer) {
            g_callbacks.on_stop_buffer();
        }
        return true;
    }
    if (msg == kWmRegionSave) {
        if (g_callbacks.on_region_save) {
            g_callbacks.on_region_save();
        }
        return true;
    }
    return false;
}

void request_quit() {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    } else {
        PostQuitMessage(0);
    }
}

}  // namespace ir::hotkey
