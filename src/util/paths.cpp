#include "util/paths.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ir::paths {
namespace {

std::string g_clips_override;

std::string wide_utf8(PWSTR wide) {
    if (!wide) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace

std::string default_clips_directory() {
    PWSTR videos = nullptr;
    std::string dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) {
        dir = wide_utf8(videos);
        CoTaskMemFree(videos);
        dir += "\\instant-replay";
    } else {
        dir = "clips";
    }
    return dir;
}

void set_clips_directory_override(const std::string& absolute_or_empty) {
    g_clips_override = absolute_or_empty;
}

std::string clips_directory() {
    std::string dir = g_clips_override.empty() ? default_clips_directory() : g_clips_override;
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string make_clip_path() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &t);

    std::ostringstream name;
    name << clips_directory() << "\\clip_" << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << ".mp4";
    return name.str();
}

std::string app_data_directory() {
    PWSTR appdata = nullptr;
    std::string dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)) && appdata) {
        dir = wide_utf8(appdata);
        CoTaskMemFree(appdata);
        dir += "\\UniyaOver";
    } else {
        dir = "UniyaOver";
    }
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string config_path() {
    return app_data_directory() + "\\config.json";
}

std::string log_path() {
    return app_data_directory() + "\\uniyaover.log";
}

}  // namespace ir::paths
