#pragma once

#include <cstdint>
#include <vector>

namespace ir::encode {

enum class VideoCodecKind {
    H264 = 0,
    Hevc = 1,
};

struct EncoderConfig {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrate = 20'000'000;
    int gop_frames = 60;
    // NVENC preset: p1 = fastest (high FPS), p5 = nicer quality @ 60.
    const char* nvenc_preset = "p5";
    VideoCodecKind codec = VideoCodecKind::H264;
};

inline const char* nvenc_encoder_name(VideoCodecKind c) {
    return c == VideoCodecKind::Hevc ? "hevc_nvenc" : "h264_nvenc";
}

inline const char* video_codec_label(VideoCodecKind c) {
    return c == VideoCodecKind::Hevc ? "HEVC" : "H.264";
}

// One compressed video packet (NAL units from NVENC).
struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 1;
    bool is_keyframe = false;
};

// Codec headers (SPS/PPS or VPS/SPS/PPS) copied after avcodec_open2.
struct CodecExtradata {
    std::vector<uint8_t> bytes;
};

}  // namespace ir::encode
