#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ir::audio {

struct AudioFormat {
    int sample_rate = 48000;
    int channels = 2;
};

// One chunk of loopback PCM with a capture timestamp (QPC).
struct PcmChunk {
    std::vector<float> interleaved;  // float32 interleaved
    int frames = 0;                  // samples per channel
    int64_t qpc = 0;                 // QPC of first sample (WASAPI)
};

class WasapiLoopback {
public:
    WasapiLoopback() = default;
    ~WasapiLoopback();

    WasapiLoopback(const WasapiLoopback&) = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    bool start();
    // Prefer a specific endpoint id; empty = auto-pick by peak/sessions.
    bool start_preferred(const std::string& device_id, bool allow_auto_switch);
    void stop();

    // Appends zero or more timestamped chunks.
    // Returns total frames (per channel) pulled this call.
    int read(std::vector<PcmChunk>& out_chunks);

    bool is_running() const { return running_; }
    const AudioFormat& format() const { return format_; }
    const std::string& device_name() const { return device_name_; }

private:
    struct Impl;
    bool open_on_device();
    bool pick_and_open();
    bool reopen_if_better_device();

    Impl* impl_ = nullptr;
    bool running_ = false;
    AudioFormat format_{};
    std::string device_name_;
    std::string device_id_;
    int silence_ticks_ = 0;
    bool allow_auto_switch_ = true;
    bool prefer_locked_ = false;
};

}  // namespace ir::audio
