#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ir::gui {

struct ClipInfo {
    std::wstring path;
    std::wstring name;
    std::wstring date_display;  // dd.mm.yyyy HH:MM
    std::wstring size_display;  // e.g. 12.4 MB
    uint64_t size_bytes = 0;
    uint64_t mtime_q = 0;  // for sort
};

// Newest first. Scans ir::paths::clips_directory() for *.mp4.
std::vector<ClipInfo> scan_clips(int max_count = 50);

std::wstring clips_dir_wide();
std::wstring free_space_display(const std::wstring& path_on_volume);

bool open_clip(const std::wstring& path);
bool reveal_clip(const std::wstring& path);
bool open_clips_folder();

}  // namespace ir::gui
