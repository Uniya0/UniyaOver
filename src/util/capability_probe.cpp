#include "util/capability_probe.h"

#include "util/log.h"

#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

using Microsoft::WRL::ComPtr;

namespace ir::hw {
namespace {

constexpr unsigned kVendorNvidia = 0x10DE;

std::string narrow_from_wide(const WCHAR* wide) {
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

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

std::string lower_copy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

GpuTier classify_nvidia(unsigned device_id, const std::string& name) {
    const std::string n = lower_copy(name);

    if (n.find("rtx 50") != std::string::npos || n.find("rtx 40") != std::string::npos ||
        n.find("rtx 30") != std::string::npos || n.find("a100") != std::string::npos ||
        n.find("a40") != std::string::npos || n.find("l40") != std::string::npos) {
        return GpuTier::AmperePlus;
    }
    if (n.find("rtx 20") != std::string::npos || n.find("gtx 16") != std::string::npos ||
        n.find("quadro rtx") != std::string::npos || n.find("titan rtx") != std::string::npos) {
        return GpuTier::Turing;
    }
    if (n.find("gtx 10") != std::string::npos || n.find("1080") != std::string::npos ||
        n.find("1070") != std::string::npos || n.find("1060") != std::string::npos ||
        n.find("1050") != std::string::npos || n.find("titan x") != std::string::npos ||
        n.find("titan xp") != std::string::npos) {
        return GpuTier::Pascal;
    }
    if (n.find("gtx 9") != std::string::npos || n.find("gtx 7") != std::string::npos) {
        return GpuTier::Pascal;
    }

    switch (device_id) {
    case 0x1B06:
    case 0x1B80:
    case 0x1B81:
    case 0x1B82:
    case 0x1C02:
    case 0x1C03:
    case 0x1C82:
        return GpuTier::Pascal;
    default:
        break;
    }

    if (!n.empty()) {
        return GpuTier::AmperePlus;
    }
    return GpuTier::Unknown;
}

std::vector<GpuAdapterInfo> enum_gpus() {
    std::vector<GpuAdapterInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ir::log::warn("Capability probe: CreateDXGIFactory1 failed");
        return out;
    }

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        GpuAdapterInfo info;
        info.index = static_cast<int>(i);
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.dedicated_vram_mb = static_cast<size_t>(desc.DedicatedVideoMemory / (1024ull * 1024ull));
        info.name = narrow_from_wide(desc.Description);
        info.is_nvidia = (desc.VendorId == kVendorNvidia);
        if (info.is_nvidia) {
            info.tier = classify_nvidia(info.device_id, info.name);
        }
        out.push_back(std::move(info));
    }
    return out;
}

bool try_open_nvenc(
    const char* encoder_name, int width, int height, int fps, std::string* err_out) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        if (err_out) {
            *err_out = std::string(encoder_name) + " not found in FFmpeg build";
        }
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        if (err_out) {
            *err_out = "avcodec_alloc_context3 failed";
        }
        return false;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{1, fps};
    ctx->framerate = AVRational{fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->bit_rate = 4'000'000;
    ctx->gop_size = fps;
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(ctx->priv_data, "preset", fps >= 144 ? "p1" : "p5", 0);
    av_opt_set(ctx->priv_data, "tune", "ll", 0);
    av_opt_set(ctx->priv_data, "rc", "vbr", 0);
    av_opt_set_int(ctx->priv_data, "cq", 23, 0);
    ctx->rc_max_rate = ctx->bit_rate;
    ctx->rc_buffer_size = static_cast<int>(ctx->bit_rate * 2);

    const int err = avcodec_open2(ctx, codec, nullptr);
    const bool ok = err >= 0;
    if (!ok && err_out) {
        *err_out = av_err_str(err);
    }
    avcodec_free_context(&ctx);
    return ok;
}

void apply_tier_and_vram_limits(HardwareProfile& p) {
    const size_t vram = p.has_nvidia ? p.nvidia.dedicated_vram_mb : 0;

    int vram_bitrate = 100;
    int vram_buffer = 120;
    if (vram > 0 && vram < 3072) {
        vram_bitrate = 40;
        vram_buffer = 30;
    } else if (vram > 0 && vram < 6144) {
        vram_bitrate = 70;
        vram_buffer = 60;
    }

    int tier_fps = p.nvenc.max_fps;
    int tier_bitrate = 100;
    switch (p.tier) {
    case GpuTier::Pascal:
        tier_fps = (std::min)(p.nvenc.max_fps > 0 ? p.nvenc.max_fps : 144, 144);
        tier_bitrate = 70;
        break;
    case GpuTier::Turing:
        tier_fps = p.nvenc.max_fps > 0 ? p.nvenc.max_fps : 240;
        tier_bitrate = 90;
        break;
    case GpuTier::AmperePlus:
    case GpuTier::Unknown:
    default:
        tier_fps = p.nvenc.max_fps > 0 ? p.nvenc.max_fps : 240;
        tier_bitrate = 100;
        break;
    }

    p.nvenc.recommended_max_fps = tier_fps;
    p.nvenc.recommended_max_bitrate_mbps = (std::min)(tier_bitrate, vram_bitrate);
    p.max_bitrate_mbps = p.nvenc.recommended_max_bitrate_mbps;
    p.max_buffer_sec = vram_buffer;

    // Prefer HEVC when it opens; H.264 otherwise.
    if (p.nvenc.hevc_opens) {
        p.nvenc.preferred_codec = VideoCodec::Hevc;
    } else {
        p.nvenc.preferred_codec = VideoCodec::H264;
    }
}

}  // namespace

const char* tier_name(GpuTier t) {
    switch (t) {
    case GpuTier::Pascal:
        return "Pascal";
    case GpuTier::Turing:
        return "Turing";
    case GpuTier::AmperePlus:
        return "Ampere+";
    default:
        return "Unknown";
    }
}

const char* codec_name(VideoCodec c) {
    return c == VideoCodec::Hevc ? "HEVC" : "H.264";
}

HardwareProfile probe() {
    HardwareProfile p;
    ir::log::info("Capability probe: scanning GPUs and NVENC…");

    p.gpus = enum_gpus();
    for (const auto& g : p.gpus) {
        char vendor_hex[16];
        std::snprintf(vendor_hex, sizeof(vendor_hex), "%04X", g.vendor_id);
        ir::log::info(
            "  GPU[" + std::to_string(g.index) + "] " + g.name +
            " vendor=0x" + vendor_hex +
            " vram=" + std::to_string(g.dedicated_vram_mb) + "MB" +
            (g.is_nvidia ? std::string(" [NVIDIA ") + tier_name(g.tier) + "]" : ""));
        if (g.is_nvidia && !p.has_nvidia) {
            p.has_nvidia = true;
            p.nvidia = g;
            p.tier = g.tier;
        }
    }

    p.nvenc.h264_encoder_present = avcodec_find_encoder_by_name("h264_nvenc") != nullptr;
    p.nvenc.hevc_encoder_present = avcodec_find_encoder_by_name("hevc_nvenc") != nullptr;

    if (!p.has_nvidia) {
        p.nvenc.detail = "NVIDIA GPU not found";
        p.capture_ready = false;
        apply_tier_and_vram_limits(p);
        ir::log::warn("Capability probe: no NVIDIA adapter — capture unavailable");
        return p;
    }

    if (!p.nvenc.h264_encoder_present) {
        p.nvenc.detail = "h264_nvenc missing from FFmpeg";
        p.capture_ready = false;
        apply_tier_and_vram_limits(p);
        ir::log::error("Capability probe: h264_nvenc not in this FFmpeg build");
        return p;
    }

    std::string err60;
    std::string err144;
    std::string err240;
    std::string err_hevc;
    p.nvenc.opens_60 =
        try_open_nvenc("h264_nvenc", p.nvenc.probe_width, p.nvenc.probe_height, 60, &err60);
    if (p.nvenc.opens_60) {
        p.nvenc.opens_144 =
            try_open_nvenc("h264_nvenc", p.nvenc.probe_width, p.nvenc.probe_height, 144, &err144);
        if (p.nvenc.opens_144) {
            p.nvenc.opens_240 =
                try_open_nvenc("h264_nvenc", p.nvenc.probe_width, p.nvenc.probe_height, 240, &err240);
        }
    }

    if (p.nvenc.hevc_encoder_present) {
        p.nvenc.hevc_opens =
            try_open_nvenc("hevc_nvenc", p.nvenc.probe_width, p.nvenc.probe_height, 60, &err_hevc);
        if (!p.nvenc.hevc_opens && !err_hevc.empty()) {
            ir::log::info("HEVC NVENC open failed: " + err_hevc);
        }
    }

    if (p.nvenc.opens_240) {
        p.nvenc.max_fps = 240;
    } else if (p.nvenc.opens_144) {
        p.nvenc.max_fps = 144;
    } else if (p.nvenc.opens_60) {
        p.nvenc.max_fps = 60;
    } else {
        p.nvenc.max_fps = 0;
    }

    p.capture_ready = p.nvenc.opens_60;
    apply_tier_and_vram_limits(p);

    std::ostringstream detail;
    detail << p.nvidia.name << " [" << tier_name(p.tier) << "]";
    if (p.capture_ready) {
        detail << " · NVENC up to " << p.nvenc.max_fps << " FPS"
               << " (rec " << p.nvenc.recommended_max_fps << ")"
               << " · prefer " << codec_name(p.nvenc.preferred_codec);
    } else {
        detail << " · NVENC open failed";
        if (!err60.empty()) {
            detail << " (" << err60 << ")";
        }
    }
    if (p.nvenc.hevc_opens) {
        detail << " · HEVC ok";
    } else if (p.nvenc.hevc_encoder_present) {
        detail << " · HEVC unavailable";
    }
    detail << " · VRAM " << p.nvidia.dedicated_vram_mb << " MB";
    p.nvenc.detail = detail.str();

    ir::log::info("Capability probe: " + p.nvenc.detail);
    if (!p.nvenc.opens_60) {
        ir::log::error(
            "NVENC H.264 could not open. Update GeForce driver or use an older FFmpeg build.");
    }

    return p;
}

std::string describe(const HardwareProfile& p) {
    std::ostringstream oss;
    oss << "nvidia=" << (p.has_nvidia ? p.nvidia.name : "none")
        << " tier=" << tier_name(p.tier)
        << " ready=" << (p.capture_ready ? "yes" : "no")
        << " max_fps=" << p.nvenc.max_fps
        << " rec_fps=" << p.nvenc.recommended_max_fps
        << " hevc_opens=" << (p.nvenc.hevc_opens ? "yes" : "no")
        << " prefer=" << codec_name(p.nvenc.preferred_codec)
        << " max_bitrate=" << p.max_bitrate_mbps << "Mbps"
        << " max_buffer=" << p.max_buffer_sec << "s";
    return oss.str();
}

std::wstring utf8_to_w(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
    return out;
}

std::wstring status_line(const HardwareProfile& p) {
    if (!p.has_nvidia) {
        return L"Железо: NVIDIA не найдена — запись недоступна";
    }
    if (!p.capture_ready) {
        return L"Железо: NVENC не открылся (обнови драйвер / FFmpeg)";
    }
    std::wstring line = L"Железо: ";
    line += utf8_to_w(p.nvidia.name);
    line += L" · до ";
    line += std::to_wstring(p.nvenc.recommended_max_fps);
    line += L" FPS";
    line += L" · ";
    line += utf8_to_w(codec_name(p.nvenc.preferred_codec));
    if (p.nvenc.hevc_opens) {
        line += L" ok";
    }
    return line;
}

bool hevc_available(const HardwareProfile& p) {
    return p.capture_ready && p.nvenc.hevc_opens;
}

bool fps_allowed(const HardwareProfile& p, int fps) {
    if (!p.capture_ready || p.nvenc.recommended_max_fps <= 0) {
        return false;
    }
    // Allow selecting up to recommended comfort FPS (tier), not just raw open max.
    const int cap = p.nvenc.recommended_max_fps;
    return fps <= cap && (fps == 60 || fps == 144 || fps == 240);
}

VideoCodec resolve_codec(const ir::settings::AppSettings& s, const HardwareProfile& p) {
    const std::string& pref = s.video_codec;
    if (pref == "h264") {
        return VideoCodec::H264;
    }
    if (pref == "hevc") {
        if (hevc_available(p)) {
            return VideoCodec::Hevc;
        }
        return VideoCodec::H264;
    }
    // auto
    return p.nvenc.preferred_codec;
}

void clamp_settings(ir::settings::AppSettings& s, const HardwareProfile& p) {
    if (s.video_codec != "auto" && s.video_codec != "h264" && s.video_codec != "hevc") {
        s.video_codec = "auto";
    }
    if (s.video_codec == "hevc" && !hevc_available(p)) {
        ir::log::warn("video_codec=hevc not available — falling back to auto");
        s.video_codec = "auto";
    }

    const int soft_max = p.nvenc.recommended_max_fps > 0
        ? p.nvenc.recommended_max_fps
        : p.nvenc.max_fps;

    if (p.capture_ready) {
        if (s.fps_cap != 60 && s.fps_cap != 144 && s.fps_cap != 240) {
            s.fps_cap = 60;
        }
        if (soft_max > 0 && s.fps_cap > soft_max) {
            const int old = s.fps_cap;
            if (soft_max >= 144) {
                s.fps_cap = 144;
            } else {
                s.fps_cap = 60;
            }
            if (soft_max == 240 || soft_max == 144 || soft_max == 60) {
                s.fps_cap = soft_max;
            }
            ir::log::warn(
                "fps_cap " + std::to_string(old) + " → " + std::to_string(s.fps_cap) +
                " (recommended max " + std::to_string(soft_max) + ")");
            s.video_bitrate = ir::settings::auto_bitrate(s.fps_cap);
        }
    } else if (s.fps_cap != 60 && s.fps_cap != 144 && s.fps_cap != 240) {
        s.fps_cap = 60;
    }

    if (s.buffer_sec > p.max_buffer_sec) {
        ir::log::warn(
            "buffer_sec " + std::to_string(s.buffer_sec) + " → " +
            std::to_string(p.max_buffer_sec) + " (VRAM limit)");
        s.buffer_sec = p.max_buffer_sec;
    }
    if (s.buffer_sec < 5) {
        s.buffer_sec = 5;
    }

    const int max_bits = p.max_bitrate_mbps * 1'000'000;
    if (s.video_bitrate > max_bits) {
        ir::log::warn(
            "bitrate clamped to " + std::to_string(p.max_bitrate_mbps) + " Mbps (profile limit)");
        s.video_bitrate = max_bits;
    }
    if (s.video_bitrate <= 0) {
        s.video_bitrate = ir::settings::auto_bitrate(s.fps_cap);
        if (s.video_bitrate > max_bits) {
            s.video_bitrate = max_bits;
        }
    }
}

}  // namespace ir::hw
