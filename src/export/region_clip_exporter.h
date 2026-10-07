#pragma once

#include "encode/encoder_config.h"
#include "export/mp4_exporter.h"

#include <Windows.h>
#include <string>
#include <vector>

namespace ir::export_mp4 {

// Decode full-frame encoded packets, crop to rect, re-encode, write MP4.
bool write_region_clip(
    const std::string& path,
    const ir::encode::EncoderConfig& full_config,
    const ir::encode::CodecExtradata& full_extradata,
    const std::vector<ir::encode::EncodedPacket>& full_packets,
    RECT crop,
    const ClipAudio* audio = nullptr);

}  // namespace ir::export_mp4
