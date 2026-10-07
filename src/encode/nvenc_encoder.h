#pragma once

#include "encode/encoder_config.h"

#include <cstdint>
#include <vector>

namespace ir::encode {

// NV12 frames in -> H.264/HEVC packets out (NVENC via FFmpeg).
// Does not touch disk. Packets are pushed into the ring buffer by the caller.
class NvencEncoder {
public:
    NvencEncoder() = default;
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    bool open(const EncoderConfig& config);
    void close();

    // Converts one BGRA frame to NV12 (CPU sws) and sends it to NVENC.
    bool encode_bgra(
        const uint8_t* bgra,
        int width,
        int height,
        int stride_bytes,
        int64_t pts,
        std::vector<EncodedPacket>& out);

    // Preferred path: already-converted NV12 (e.g. from GPU VideoProcessor).
    bool encode_nv12(
        const uint8_t* y,
        int y_stride,
        const uint8_t* uv,
        int uv_stride,
        int width,
        int height,
        int64_t pts,
        std::vector<EncodedPacket>& out);

    // Drain delayed packets. Call once before destroying the encoder.
    bool flush(std::vector<EncodedPacket>& out);

    bool is_open() const { return opened_; }
    const EncoderConfig& config() const { return config_; }
    const CodecExtradata& extradata() const { return extradata_; }

private:
    bool drain_packets(std::vector<EncodedPacket>& out);
    void cleanup();

    struct Impl;
    Impl* impl_ = nullptr;
    bool opened_ = false;
    EncoderConfig config_{};
    CodecExtradata extradata_{};
};

}  // namespace ir::encode
