#pragma once

#include <chrono>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace ir::time {

// Shared capture timeline. Both video and audio PTS are derived from the same
// QueryPerformanceCounter origin — this is what keeps Instant Replay in sync.
class Timeline {
public:
    void reset() {
        LARGE_INTEGER f{};
        LARGE_INTEGER c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        freq_ = f.QuadPart > 0 ? f.QuadPart : 1;
        t0_ = c.QuadPart;
    }

    int64_t qpc_now() const {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    }

    double seconds_at(int64_t qpc) const {
        return static_cast<double>(qpc - t0_) / static_cast<double>(freq_);
    }

    double seconds_now() const { return seconds_at(qpc_now()); }

    // Video time_base = 1/fps
    int64_t video_pts_at(int64_t qpc, int fps) const {
        if (fps <= 0) {
            return 0;
        }
        // Round to nearest frame tick.
        return static_cast<int64_t>(seconds_at(qpc) * fps + 0.5);
    }

    // Audio time_base = 1/sample_rate
    int64_t audio_pts_at(int64_t qpc, int sample_rate) const {
        if (sample_rate <= 0) {
            return 0;
        }
        return static_cast<int64_t>(seconds_at(qpc) * sample_rate + 0.5);
    }

    int64_t t0_qpc() const { return t0_; }
    int64_t freq() const { return freq_; }

    // Convert WASAPI QPC timestamp (same domain as QueryPerformanceCounter).
    static int64_t qpc_from_wasapi(UINT64 wasapi_qpc) {
        return static_cast<int64_t>(wasapi_qpc);
    }

private:
    int64_t freq_ = 1;
    int64_t t0_ = 0;
};

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint now() {
    return Clock::now();
}

inline double seconds_between(TimePoint start, TimePoint end) {
    return std::chrono::duration<double>(end - start).count();
}

}  // namespace ir::time
