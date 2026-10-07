#pragma once

#include "encode/audio_config.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ir::buffer {

// Ring of AAC packets, trimmed by sample duration (~30 seconds).
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(int64_t max_duration_samples);

    void push(ir::encode::AudioPacket packet);
    void clear();

    struct Snapshot {
        std::vector<ir::encode::AudioPacket> packets;
        int64_t duration_samples = 0;
        std::size_t bytes = 0;
    };

    Snapshot snapshot() const;

    // Keep only packets that overlap [start_sec, end_sec) on the shared timeline.
    // Audio pts is in 1/sample_rate ticks; sample_rate converts to seconds.
    Snapshot snapshot_trimmed(double start_sec, double end_sec, int sample_rate) const;

    // Trim by sample indices [start_sample, end_sample) on the session timeline.
    Snapshot snapshot_samples(int64_t start_sample, int64_t end_sample) const;

    int64_t duration_samples() const;
    std::size_t bytes() const;

private:
    void drop_overflow_locked();

    const int64_t max_duration_samples_;
    mutable std::mutex mutex_;
    std::deque<ir::encode::AudioPacket> packets_;
    int64_t duration_samples_ = 0;
    std::size_t bytes_ = 0;
};

}  // namespace ir::buffer
