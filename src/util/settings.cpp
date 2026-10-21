#include "util/settings.h"

#include "util/log.h"
#include "util/paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace ir::settings {
namespace {

bool starts_with(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool extract_int(const std::string& body, const char* key, int& out) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = body.find(needle);
    if (p == std::string::npos) {
        return false;
    }
    const size_t colon = body.find(':', p + needle.size());
    if (colon == std::string::npos) {
        return false;
    }
    out = std::atoi(body.c_str() + colon + 1);
    return true;
}

bool extract_string(const std::string& body, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = body.find(needle);
    if (p == std::string::npos) {
        return false;
    }
    const size_t colon = body.find(':', p + needle.size());
    if (colon == std::string::npos) {
        return false;
    }
    const size_t q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) {
        return false;
    }
    std::string value;
    for (size_t i = q1 + 1; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            value.push_back(body[i + 1]);
            ++i;
            continue;
        }
        if (body[i] == '"') {
            out = value;
            return true;
        }
        value.push_back(body[i]);
    }
    return false;
}

void apply_cli(AppSettings& s, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (starts_with(a, "--fps=")) {
            const int v = std::atoi(a + 6);
            if (v == 60 || v == 144 || v == 240) {
                s.fps_cap = v;
            } else {
                ir::log::warn(
                    "Unsupported --fps= (use 60, 144, or 240); keeping " +
                    std::to_string(s.fps_cap));
            }
        } else if (starts_with(a, "--bitrate=")) {
            const int mbps = std::atoi(a + 10);
            if (mbps >= 5 && mbps <= 100) {
                s.video_bitrate = mbps * 1'000'000;
            } else {
                ir::log::warn("--bitrate= expects 5..100 (Mbps)");
            }
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            ir::log::info("Usage: instant_replay [--fps=60|144|240] [--bitrate=Mbps] [--console]");
        }
    }
}

bool load_file(AppSettings& s) {
    const std::string path = ir::paths::config_path();
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string body = ss.str();

    extract_int(body, "fps_cap", s.fps_cap);
    extract_int(body, "buffer_sec", s.buffer_sec);
    extract_int(body, "video_bitrate", s.video_bitrate);
    extract_string(body, "video_codec", s.video_codec);
    int v = 0;
    if (extract_int(body, "hk_arm_mod", v)) {
        s.hk_arm.modifiers = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_arm_vk", v)) {
        s.hk_arm.vk = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_save_mod", v)) {
        s.hk_save.modifiers = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_save_vk", v)) {
        s.hk_save.vk = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_stop_mod", v)) {
        s.hk_stop.modifiers = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_stop_vk", v)) {
        s.hk_stop.vk = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_region_mod", v)) {
        s.hk_region.modifiers = static_cast<unsigned>(v);
    }
    if (extract_int(body, "hk_region_vk", v)) {
        s.hk_region.vk = static_cast<unsigned>(v);
    }
    extract_int(body, "monitor_index", s.monitor_index);
    extract_string(body, "audio_device_id", s.audio_device_id);
    extract_string(body, "clips_folder", s.clips_folder);
    if (extract_int(body, "toast_enabled", v)) {
        s.toast_enabled = (v != 0);
    }
    extract_int(body, "toast_position", s.toast_position);
    extract_int(body, "toast_size", s.toast_size);
    extract_string(body, "language", s.language);
    if (s.language != "ru" && s.language != "en") {
        s.language.clear();
    }
    if (s.toast_position < 0 || s.toast_position > 5) {
        s.toast_position = 2;
    }
    if (s.toast_size < 0 || s.toast_size > 2) {
        s.toast_size = 1;
    }

    // Existing installs without the key should not be forced into the tour again.
    if (body.find("\"onboarding_done\"") == std::string::npos) {
        s.onboarding_done = true;
    } else {
        int ob = 0;
        if (extract_int(body, "onboarding_done", ob)) {
            s.onboarding_done = (ob != 0);
        }
    }

    if (s.fps_cap != 60 && s.fps_cap != 144 && s.fps_cap != 240) {
        s.fps_cap = 144;
    }
    if (s.video_codec != "auto" && s.video_codec != "h264" && s.video_codec != "hevc") {
        s.video_codec = "auto";
    }
    if (s.buffer_sec < 5) {
        s.buffer_sec = 5;
    }
    if (s.buffer_sec > 120) {
        s.buffer_sec = 120;
    }
    if (s.hk_arm.vk == 0) {
        s.hk_arm = {MOD_CONTROL | MOD_SHIFT, 'R'};
    }
    if (s.hk_save.vk == 0) {
        s.hk_save = {MOD_CONTROL | MOD_SHIFT, 'X'};
    }
    if (s.hk_stop.vk == 0) {
        s.hk_stop = {MOD_CONTROL | MOD_SHIFT, 'S'};
    }
    if (s.hk_region.vk == 0) {
        s.hk_region = {MOD_CONTROL | MOD_SHIFT, 'A'};
    }
    return true;
}

}  // namespace

int auto_bitrate(int fps_cap) {
    if (fps_cap >= 240) {
        return 50'000'000;
    }
    if (fps_cap >= 144) {
        return 35'000'000;
    }
    return 20'000'000;
}

int auto_gop_frames(int fps_cap) {
    return fps_cap > 0 ? fps_cap : 60;
}

const char* nvenc_preset_for_fps(int fps_cap) {
    if (fps_cap >= 240) {
        return "p1";
    }
    if (fps_cap >= 144) {
        return "p2";
    }
    return "p5";
}

AppSettings from_args(int argc, char** argv) {
    AppSettings s;
    apply_cli(s, argc, argv);
    if (s.video_bitrate <= 0) {
        s.video_bitrate = auto_bitrate(s.fps_cap);
    }
    return s;
}

AppSettings load(int argc, char** argv) {
    AppSettings s;
    if (load_file(s)) {
        ir::log::info("Loaded config: " + ir::paths::config_path());
    } else {
        ir::log::info("No config yet — using defaults");
    }
    apply_cli(s, argc, argv);
    if (s.video_bitrate <= 0) {
        s.video_bitrate = auto_bitrate(s.fps_cap);
    }
    return s;
}

bool save(const AppSettings& s) {
    const std::string path = ir::paths::config_path();
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        ir::log::error("Failed to write config: " + path);
        return false;
    }
    out << "{\n"
        << "  \"fps_cap\": " << s.fps_cap << ",\n"
        << "  \"buffer_sec\": " << s.buffer_sec << ",\n"
        << "  \"video_bitrate\": " << s.video_bitrate << ",\n"
        << "  \"video_codec\": \"" << json_escape(s.video_codec) << "\",\n"
        << "  \"hk_arm_mod\": " << s.hk_arm.modifiers << ",\n"
        << "  \"hk_arm_vk\": " << s.hk_arm.vk << ",\n"
        << "  \"hk_save_mod\": " << s.hk_save.modifiers << ",\n"
        << "  \"hk_save_vk\": " << s.hk_save.vk << ",\n"
        << "  \"hk_stop_mod\": " << s.hk_stop.modifiers << ",\n"
        << "  \"hk_stop_vk\": " << s.hk_stop.vk << ",\n"
        << "  \"hk_region_mod\": " << s.hk_region.modifiers << ",\n"
        << "  \"hk_region_vk\": " << s.hk_region.vk << ",\n"
        << "  \"monitor_index\": " << s.monitor_index << ",\n"
        << "  \"audio_device_id\": \"" << json_escape(s.audio_device_id) << "\",\n"
        << "  \"clips_folder\": \"" << json_escape(s.clips_folder) << "\",\n"
        << "  \"toast_enabled\": " << (s.toast_enabled ? 1 : 0) << ",\n"
        << "  \"toast_position\": " << s.toast_position << ",\n"
        << "  \"toast_size\": " << s.toast_size << ",\n"
        << "  \"language\": \"" << json_escape(s.language) << "\",\n"
        << "  \"onboarding_done\": " << (s.onboarding_done ? 1 : 0) << "\n"
        << "}\n";
    ir::log::info("Saved config: " + path);
    return true;
}

std::string hotkey_label(const HotkeyBinding& b) {
    std::string out;
    if (b.modifiers & MOD_CONTROL) {
        out += "Ctrl+";
    }
    if (b.modifiers & MOD_SHIFT) {
        out += "Shift+";
    }
    if (b.modifiers & MOD_ALT) {
        out += "Alt+";
    }
    if (b.modifiers & MOD_WIN) {
        out += "Win+";
    }
    if (b.vk == 0) {
        return out;
    }
    if (b.vk >= 'A' && b.vk <= 'Z') {
        out.push_back(static_cast<char>(b.vk));
    } else if (b.vk >= '0' && b.vk <= '9') {
        out.push_back(static_cast<char>(b.vk));
    } else if (b.vk >= VK_F1 && b.vk <= VK_F12) {
        out += "F" + std::to_string(b.vk - VK_F1 + 1);
    } else if (b.vk == VK_SPACE) {
        out += "Space";
    } else if (b.vk == VK_TAB) {
        out += "Tab";
    } else if (b.vk == VK_RETURN) {
        out += "Enter";
    } else if (b.vk == VK_BACK) {
        out += "Backspace";
    } else if (b.vk == VK_DELETE) {
        out += "Delete";
    } else if (b.vk == VK_INSERT) {
        out += "Insert";
    } else if (b.vk == VK_HOME) {
        out += "Home";
    } else if (b.vk == VK_END) {
        out += "End";
    } else if (b.vk == VK_PRIOR) {
        out += "PageUp";
    } else if (b.vk == VK_NEXT) {
        out += "PageDown";
    } else if (b.vk == VK_LEFT) {
        out += "Left";
    } else if (b.vk == VK_RIGHT) {
        out += "Right";
    } else if (b.vk == VK_UP) {
        out += "Up";
    } else if (b.vk == VK_DOWN) {
        out += "Down";
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "VK_%u", b.vk);
        out += buf;
    }
    return out;
}

std::string describe(const AppSettings& s) {
    std::ostringstream oss;
    oss << "fps_cap=" << s.fps_cap
        << " buffer=" << s.buffer_sec << "s"
        << " bitrate=" << (s.video_bitrate / 1000) << " kbps"
        << " codec=" << s.video_codec
        << " gop=" << auto_gop_frames(s.fps_cap)
        << " nvenc_preset=" << nvenc_preset_for_fps(s.fps_cap)
        << " monitor=" << s.monitor_index
        << " arm=" << hotkey_label(s.hk_arm)
        << " save=" << hotkey_label(s.hk_save)
        << " region=" << hotkey_label(s.hk_region)
        << " stop=" << hotkey_label(s.hk_stop);
    return oss.str();
}

}  // namespace ir::settings
