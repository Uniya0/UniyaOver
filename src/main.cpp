#include "capture/capture_session.h"
#include "gui/app_window.h"
#include "gui/gdi_utils.h"
#include "util/capability_probe.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shellapi.h>

#include <string>
#include <vector>

namespace {

bool has_flag(const std::vector<std::string>& args, const char* flag) {
    for (const auto& a : args) {
        if (a == flag) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> args_from_command_line() {
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    if (!argv_w) {
        return out;
    }
    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.push_back(ir::gui::wide_to_utf8(argv_w[i]));
    }
    LocalFree(argv_w);
    return out;
}

}  // namespace

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE /*prev*/,
    LPWSTR /*cmd*/,
    int /*show*/) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const auto arg_strings = args_from_command_line();
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(arg_strings.size());
    for (auto& s : const_cast<std::vector<std::string>&>(arg_strings)) {
        argv_ptrs.push_back(s.data());
    }

    if (has_flag(arg_strings, "--console")) {
        ir::log::attach_console();
    }
    ir::log::set_file(ir::paths::log_path());

    ir::settings::AppSettings settings =
        ir::settings::load(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

    const ir::hw::HardwareProfile hw = ir::hw::probe();
    ir::hw::clamp_settings(settings, hw);
    ir::settings::save(settings);

    ir::log::info("UniyaOver v1.1.0 — Instant Replay (NVENC + WASAPI/AAC)");
    ir::log::info("Hardware: " + ir::hw::describe(hw));
    ir::log::info(ir::settings::describe(settings));

    ir::capture::CaptureSession capture;
    capture.set_hardware_profile(hw);
    capture.set_settings(settings);

    ir::gui::AppWindow window;
    if (!window.create(instance, &capture, settings, hw)) {
        ir::log::error("Failed to create UniyaOver window");
        CoUninitialize();
        return 1;
    }

    const int code = window.run_message_loop();
    capture.stop();
    CoUninitialize();
    ir::log::info("Exited cleanly");
    return code;
}
