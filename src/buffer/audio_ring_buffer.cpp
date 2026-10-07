#include "buffer/audio_ring_buffer.h"

namespace ir::buffer {

AudioRingBuffer::AudioRingBuffer(int64_t max_duration_samples)
    : max_duration_samples_(max_duration_samples) {}

void AudioRingBuffer::push(ir::encode::AudioPacket packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    duration_samples_ += packet.duration;
    bytes_ += packet.data.size();
    packets_.push_back(std::move(packet));
    drop_overflow_locked();
}

void AudioRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    duration_samples_ = 0;
    bytes_ = 0;
}

AudioRingBuffer::Snapshot AudioRingBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    snap.packets.assign(packets_.begin(), packets_.end());
    snap.duration_samples = duration_samples_;
    snap.bytes = bytes_;
    return snap;
}

AudioRingBuffer::Snapshot AudioRingBuffer::snapshot_samples(
    int64_t start_sample,
    int64_t end_sample) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    if (end_sample <= start_sample) {
        return snap;
    }

    for (const auto& pkt : packets_) {
        const int64_t pkt_end = pkt.pts + pkt.duration;
        if (pkt_end <= start_sample) {
            continue;
        }
        if (pkt.pts >= end_sample) {
            break;
        }
        snap.packets.push_back(pkt);
        snap.duration_samples += pkt.duration;
        snap.bytes += pkt.data.size();
    }
    return snap;
}

AudioRingBuffer::Snapshot AudioRingBuffer::snapshot_trimmed(
    double start_sec,
    double end_sec,
    int sample_rate) const {
    if (sample_rate <= 0 || end_sec <= start_sec) {
        return {};
    }
    const int64_t start_sample = static_cast<int64_t>(start_sec * sample_rate);
    const int64_t end_sample = static_cast<int64_t>(end_sec * sample_rate);
    return snapshot_samples(start_sample, end_sample);
}

int64_t AudioRingBuffer::duration_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duration_samples_;
}

std::size_t AudioRingBuffer::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

void AudioRingBuffer::drop_overflow_locked() {
    // Trim by timeline PTS span (1/sample_rate) so A/V share the same ~N seconds window.
    while (!packets_.empty()) {
        const int64_t first = packets_.front().pts;
        const int64_t last = packets_.back().pts + packets_.back().duration;
        if ((last - first) <= max_duration_samples_) {
            break;
        }
        duration_samples_ -= packets_.front().duration;
        bytes_ -= packets_.front().data.size();
        packets_.pop_front();
    }
}

}  // namespace ir::buffer
