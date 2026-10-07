#pragma once

#include "encode/audio_config.h"
#include "encode/encoder_config.h"

#include <cstdint>
#include <vector>

namespace ir::encode {

// Interleaved float32 PCM in -> AAC packets out (FFmpeg native AAC).
class AacEncoder {
public:
    AacEncoder() = default;
    ~AacEncoder();

    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    bool open(const AudioEncoderConfig& config);
    void close();

    // `first_sample_pts` = timeline sample index of interleaved[0] (1/sample_rate ticks).
    bool encode_interleaved_f32(
        const float* interleaved,
        int sample_count,
        int64_t first_sample_pts,
        std::vector<AudioPacket>& out);

    bool flush(std::vector<AudioPacket>& out);

    bool is_open() const { return opened_; }
    const AudioEncoderConfig& config() const { return config_; }
    const CodecExtradata& extradata() const { return extradata_; }
    int frame_samples() const { return frame_samples_; }
    // AAC priming samples — subtracted from packet PTS so A/V share wall-clock.
    int encoder_delay_samples() const { return encoder_delay_; }

private:
    bool drain_packets(std::vector<AudioPacket>& out);
    bool encode_full_frames(std::vector<AudioPacket>& out);
    void cleanup();

    struct Impl;
    Impl* impl_ = nullptr;
    bool opened_ = false;
    AudioEncoderConfig config_{};
    CodecExtradata extradata_{};
    int frame_samples_ = 1024;
    int encoder_delay_ = 0;
    std::vector<float> pending_;  // interleaved leftover samples
    int64_t pending_pts_ = 0;     // timeline pts of pending_[0]
    bool have_pending_pts_ = false;
};

}  // namespace ir::encode
