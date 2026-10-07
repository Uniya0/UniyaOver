#pragma once

#include "encode/audio_config.h"
#include "encode/encoder_config.h"

#include <string>
#include <vector>

namespace ir::export_mp4 {

struct ClipAudio {
    ir::encode::AudioEncoderConfig config;
    ir::encode::CodecExtradata extradata;
    std::vector<ir::encode::AudioPacket> packets;
};

// Remux already-encoded packets into a standalone MP4.
// Video timestamps are in 1/fps ticks. Audio timestamps are in 1/sample_rate ticks.
// Both are rewritten so the file starts near PTS 0. Audio is optional.
bool write_clip(
    const std::string& path,
    const ir::encode::EncoderConfig& video_config,
    const ir::encode::CodecExtradata& video_extradata,
    const std::vector<ir::encode::EncodedPacket>& video_packets,
    const ClipAudio* audio = nullptr);

}  // namespace ir::export_mp4
