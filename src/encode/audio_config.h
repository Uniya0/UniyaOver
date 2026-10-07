#pragma once

#include <cstdint>
#include <vector>

namespace ir::encode {

struct AudioEncoderConfig {
    int sample_rate = 48000;
    int channels = 2;
    int bitrate = 192000;  // 192 kbps AAC
};

// One compressed AAC packet.
struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t pts = 0;       // in 1/sample_rate ticks (samples)
    int64_t duration = 0;  // samples covered by this packet
};

}  // namespace ir::encode
