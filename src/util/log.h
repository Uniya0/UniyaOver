#pragma once

#include <string>

namespace ir::log {

enum class Level {
    Info,
    Warn,
    Error,
};

// Optional: tee logs to a file (UTF-8). Safe to call before other log APIs.
void set_file(const std::string& path);
// Attach a console for debugging (e.g. --console).
void attach_console();

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

}  // namespace ir::log
