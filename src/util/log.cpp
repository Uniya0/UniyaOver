#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <io.h>
#include <fcntl.h>

namespace ir::log {
namespace {

std::mutex g_mutex;
std::string g_file_path;
bool g_console = false;

const char* level_tag(Level level) {
    switch (level) {
        case Level::Info:
            return "INFO";
        case Level::Warn:
            return "WARN";
        case Level::Error:
            return "ERROR";
    }
    return "INFO";
}

void write(Level level, const std::string& message) {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);

    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif

    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &local_tm);

    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string line =
        std::string("[") + time_buf + "] [" + level_tag(level) + "] " + message;

    if (g_console) {
        std::ostream& out = (level == Level::Error) ? std::cerr : std::cout;
        out << line << '\n';
        out.flush();
    }

    if (!g_file_path.empty()) {
        std::ofstream f(g_file_path, std::ios::app);
        if (f) {
            f << line << '\n';
        }
    }

    OutputDebugStringA((line + "\n").c_str());
}

}  // namespace

void set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_file_path = path;
}

void attach_console() {
    if (g_console) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
    g_console = true;
}

void info(const std::string& message) {
    write(Level::Info, message);
}

void warn(const std::string& message) {
    write(Level::Warn, message);
}

void error(const std::string& message) {
    write(Level::Error, message);
}

}  // namespace ir::log
