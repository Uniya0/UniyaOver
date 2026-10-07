#pragma once

#include "capture/desktop_duplicator.h"
#include "encode/audio_config.h"
#include "encode/encoder_config.h"
#include "export/mp4_exporter.h"
#include "util/capability_probe.h"
#include "util/settings.h"
#include "util/time.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ir::audio {
class WasapiLoopback;
}
namespace ir::encode {
class AacEncoder;
}
namespace ir::buffer {
class AudioRingBuffer;
}

namespace ir::capture {

class CaptureSession {
public:
    CaptureSession() = default;
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void set_settings(const ir::settings::AppSettings& settings) { settings_ = settings; }
    void set_hardware_profile(const ir::hw::HardwareProfile& profile) { hw_ = profile; }
    const ir::hw::HardwareProfile& hardware_profile() const { return hw_; }

    bool arm();
    void stop();

    bool is_armed() const { return ready_.load(); }

    bool request_save();

    // Region flow: freeze ring at hotkey → user picks crop → complete/cancel.
    bool freeze_region_buffer();
    void cancel_region_freeze();
    bool complete_region_save(RECT crop);
    bool is_region_picking() const { return region_picking_.load(); }

    int capture_width() const;
    int capture_height() const;
    RECT capture_desktop_rect() const;

    CaptureStats snapshot_stats() const;
    std::string last_clip_path() const;

private:
    struct FrozenClip {
        ir::encode::EncoderConfig enc_cfg{};
        ir::encode::CodecExtradata vextra{};
        std::vector<ir::encode::EncodedPacket> packets;
        ir::export_mp4::ClipAudio audio{};
        bool have_audio = false;
    };

    void video_thread_main();
    void audio_thread_main(
        ir::audio::WasapiLoopback* loopback,
        ir::encode::AacEncoder* aac,
        ir::buffer::AudioRingBuffer* audio_ring,
        const ir::encode::AudioEncoderConfig* aac_cfg,
        const ir::time::Timeline* timeline,
        std::atomic<uint64_t>* pcm_frames_total);
    void join_save_thread();
    void signal_init(bool ok);
    void clear_frozen_locked();

    ir::settings::AppSettings settings_{};
    ir::hw::HardwareProfile hw_{};

    DesktopDuplicator duplicator_;
    std::thread video_thread_;
    std::thread audio_thread_;
    std::thread save_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> region_picking_{false};

    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    bool init_done_ = false;
    bool init_ok_ = false;

    mutable std::mutex mutex_;
    CaptureStats stats_{};
    std::string last_clip_path_;
    bool save_requested_ = false;
    bool region_freeze_requested_ = false;
    bool freeze_done_ = false;
    bool freeze_ok_ = false;
    std::condition_variable freeze_cv_;
    std::unique_ptr<FrozenClip> frozen_;
    std::atomic<bool> save_in_progress_{false};
};

}  // namespace ir::capture
