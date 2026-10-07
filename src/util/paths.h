#pragma once

#include <string>

namespace ir::paths {

// Default: %USERPROFILE%\Videos\instant-replay
std::string default_clips_directory();

// Active clips folder (override if set, else default). Ensures directory exists.
std::string clips_directory();

// Empty clears override (back to default).
void set_clips_directory_override(const std::string& absolute_or_empty);

std::string make_clip_path();

// %LOCALAPPDATA%/UniyaOver/
std::string app_data_directory();
std::string config_path();
std::string log_path();

}  // namespace ir::paths
