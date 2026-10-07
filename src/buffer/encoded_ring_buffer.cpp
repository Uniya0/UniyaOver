#include "buffer/encoded_ring_buffer.h"

namespace ir::buffer {

EncodedRingBuffer::EncodedRingBuffer(int64_t max_duration_tb)
    : max_duration_tb_(max_duration_tb) {}

void EncodedRingBuffer::push(ir::encode::EncodedPacket packet) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Wait for the first keyframe so the ring never starts mid-GOP.
    if (gops_.empty() && current_.packets.empty() && !packet.is_keyframe) {
        return;
    }

    if (packet.is_keyframe && !current_.packets.empty()) {
        gops_.push_back(std::move(current_));
        current_ = Gop{};
    }

    current_.duration_tb += packet.duration;
    current_.bytes += packet.data.size();
    duration_tb_ += packet.duration;
    bytes_ += packet.data.size();
    current_.packets.push_back(std::move(packet));

    drop_overflow_locked();
}

void EncodedRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    gops_.clear();
    current_ = Gop{};
    duration_tb_ = 0;
    bytes_ = 0;
}

EncodedRingBuffer::Snapshot EncodedRingBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Snapshot snap;
    snap.duration_tb = duration_tb_;
    snap.bytes = bytes_;

    std::size_t packet_count = current_.packets.size();
    for (const auto& gop : gops_) {
        packet_count += gop.packets.size();
    }
    snap.packets.reserve(packet_count);

    for (const auto& gop : gops_) {
        for (const auto& pkt : gop.packets) {
            snap.packets.push_back(pkt);
        }
    }
    for (const auto& pkt : current_.packets) {
        snap.packets.push_back(pkt);
    }
    return snap;
}

int64_t EncodedRingBuffer::duration_tb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duration_tb_;
}

std::size_t EncodedRingBuffer::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

std::size_t EncodedRingBuffer::gop_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = gops_.size();
    if (!current_.packets.empty()) {
        n += 1;
    }
    return n;
}

void EncodedRingBuffer::drop_overflow_locked() {
    // Trim by PTS span on the shared timeline so "last N seconds" is wall-clock,
    // not "N seconds of encoded frames" (lag drops leave PTS holes).
    while (!gops_.empty()) {
        if (gops_.front().packets.empty()) {
            gops_.pop_front();
            continue;
        }
        const int64_t first_pts = gops_.front().packets.front().pts;
        int64_t last_pts = first_pts;
        if (!current_.packets.empty()) {
            last_pts = current_.packets.back().pts;
        } else if (!gops_.back().packets.empty()) {
            last_pts = gops_.back().packets.back().pts;
        }
        if ((last_pts - first_pts) <= max_duration_tb_) {
            break;
        }
        duration_tb_ -= gops_.front().duration_tb;
        bytes_ -= gops_.front().bytes;
        gops_.pop_front();
    }
}

}  // namespace ir::buffer
