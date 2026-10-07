#pragma once

#include "encode/encoder_config.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ir::buffer {

// Ring of already-compressed H.264 packets, stored as whole GOPs.
//
// Why GOP units, not a byte ring?
// Cutting mid-GOP drops the keyframe that later P-frames need, so the MP4
// looks corrupt. Dropping the oldest GOP keeps every remaining packet
// independently decodable from the start of the snapshot.
class EncodedRingBuffer {
public:
    explicit EncodedRingBuffer(int64_t max_duration_tb);

    void push(ir::encode::EncodedPacket packet);
    void clear();

    struct Snapshot {
        std::vector<ir::encode::EncodedPacket> packets;
        int64_t duration_tb = 0;
        std::size_t bytes = 0;
    };

    Snapshot snapshot() const;

    int64_t duration_tb() const;
    std::size_t bytes() const;
    std::size_t gop_count() const;

private:
    struct Gop {
        std::vector<ir::encode::EncodedPacket> packets;
        int64_t duration_tb = 0;
        std::size_t bytes = 0;
    };

    void drop_overflow_locked();

    const int64_t max_duration_tb_;
    mutable std::mutex mutex_;
    std::deque<Gop> gops_;
    Gop current_;
    int64_t duration_tb_ = 0;
    std::size_t bytes_ = 0;
};

}  // namespace ir::buffer
