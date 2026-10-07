#pragma once

#include "util/settings.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ir::hw {

enum class GpuTier {
    Unknown = 0,
    Pascal,   // GTX 10-series
    Turing,   // RTX 20 / GTX 16
    AmperePlus,  // RTX 30 / 40 / 50-ish and newer
};

enum class VideoCodec {
    H264 = 0,
    Hevc = 1,
};

struct GpuAdapterInfo {
    int index = 0;
    unsigned vendor_id = 0;
    unsigned device_id = 0;
    size_t dedicated_vram_mb = 0;
    std::string name;
    bool is_nvidia = false;
    GpuTier tier = GpuTier::Unknown;
};

struct NvencCaps {
    bool h264_encoder_present = false;
    bool hevc_encoder_present = false;
    bool opens_60 = false;
    bool opens_144 = false;
    bool opens_240 = false;
    bool hevc_opens = false;
    int max_fps = 0;               // hard open limit: 0 | 60 | 144 | 240
    int recommended_max_fps = 0;   // comfort limit by GPU tier
    int recommended_max_bitrate_mbps = 100;
    VideoCodec preferred_codec = VideoCodec::H264;
    int probe_width = 1280;
    int probe_height = 720;
    std::string detail;
};

struct HardwareProfile {
    std::vector<GpuAdapterInfo> gpus;
    bool has_nvidia = false;
    GpuAdapterInfo nvidia{};
    GpuTier tier = GpuTier::Unknown;
    NvencCaps nvenc{};
    int max_bitrate_mbps = 100;
    int max_buffer_sec = 120;
    bool capture_ready = false;
};

HardwareProfile probe();

std::string describe(const HardwareProfile& p);
std::wstring status_line(const HardwareProfile& p);
const char* tier_name(GpuTier t);
const char* codec_name(VideoCodec c);

bool fps_allowed(const HardwareProfile& p, int fps);
bool hevc_available(const HardwareProfile& p);

// Resolve user preference (auto/h264/hevc) against what this PC can open.
VideoCodec resolve_codec(const ir::settings::AppSettings& s, const HardwareProfile& p);

void clamp_settings(ir::settings::AppSettings& s, const HardwareProfile& p);

}  // namespace ir::hw
