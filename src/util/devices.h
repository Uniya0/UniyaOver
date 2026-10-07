#pragma once

#include "util/settings.h"

#include <functional>
#include <string>
#include <vector>

namespace ir::devices {

struct MonitorInfo {
    int index = 0;           // 0-based across attached outputs
    bool is_primary = false;
    int width = 0;
    int height = 0;
    std::string name;        // e.g. \\.\DISPLAY1 or device name
    std::wstring label;      // UI label
};

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
    std::wstring label;
};

std::vector<MonitorInfo> list_monitors();
std::vector<AudioDeviceInfo> list_playback_devices();

}  // namespace ir::devices
