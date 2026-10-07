#include "capture/capture_session.h"

#include "audio/wasapi_loopback.h"
#include "buffer/audio_ring_buffer.h"
#include "buffer/encoded_ring_buffer.h"
#include "capture/gpu_nv12_converter.h"
#include "encode/aac_encoder.h"
#include "encode/nvenc_encoder.h"
#include "export/mp4_exporter.h"
#include "export/region_clip_exporter.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/settings.h"
#include "util/time.h"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

namespace ir::capture {
namespace {

unsigned ms_until(ir::time::TimePoint deadline) {
    const double sec = ir::time::seconds_between(ir::time::now(), deadline);
    if (sec <= 0.0) {
        return 0;
    }
    const int ms = static_cast<int>(sec * 1000.0);
    if (ms < 1) {
        return 1;
    }
    return static_cast<unsigned>(ms);
}

void sleep_until(ir::time::TimePoint deadline) {
    const unsigned ms = ms_until(deadline);
    if (ms > 0) {
        Sleep(ms);
    }
}

}  // namespace

CaptureSession::~CaptureSession() {
    stop();
}

void CaptureSession::signal_init(bool ok) {
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_ok_ = ok;
        init_done_ = true;
        ready_.store(ok);
    }
    init_cv_.notify_all();
}

bool CaptureSession::arm() {
    if (ready_.load()) {
        ir::log::info("Already buffering (~30s rolling in RAM). Ctrl+Shift+X to save.");
        return true;
    }
    if (running_.load()) {
        std::unique_lock<std::mutex> lock(init_mutex_);
        init_cv_.wait(lock, [this] { return init_done_; });
        return init_ok_;
    }

    if (!hw_.capture_ready) {
        ir::log::error(
            "Cannot arm: hardware profile reports NVENC unavailable. "
            "Update NVIDIA driver or check FFmpeg NVENC build.");
        return false;
    }
    if (!ir::hw::fps_allowed(hw_, settings_.fps_cap)) {
        ir::log::error(
            "Cannot arm: fps_cap=" + std::to_string(settings_.fps_cap) +
            " exceeds hardware max " + std::to_string(hw_.nvenc.max_fps));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_done_ = false;
        init_ok_ = false;
    }
    ready_.store(false);
    running_.store(true);
    video_thread_ = std::thread([this] { video_thread_main(); });

    std::unique_lock<std::mutex> lock(init_mutex_);
    init_cv_.wait(lock, [this] { return init_done_; });
    if (!init_ok_) {
        running_.store(false);
        if (video_thread_.joinable()) {
            video_thread_.join();
        }
        return false;
    }
    ir::log::info("State: BUFFERING (Ctrl+Shift+X = save, Ctrl+Shift+S = stop)");
    return true;
}

void CaptureSession::stop() {
    if (!running_.load() && !video_thread_.joinable()) {
        ir::log::info("State: IDLE (already not buffering)");
        return;
    }
    ir::log::info("Stopping buffer — releasing capture/encode and clearing rings...");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_frozen_locked();
        freeze_done_ = true;
        freeze_ok_ = false;
    }
    freeze_cv_.notify_all();
    region_picking_.store(false);
    running_.store(false);
    if (video_thread_.joinable()) {
        video_thread_.join();
    }
    ready_.store(false);
    join_save_thread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = {};
        last_clip_path_.clear();
        save_requested_ = false;
    }
    ir::log::info("State: IDLE (not buffering, RAM freed)");
}

void CaptureSession::join_save_thread() {
    if (save_thread_.joinable()) {
        save_thread_.join();
    }
}

bool CaptureSession::request_save() {
    if (!ready_.load() || !running_.load()) {
        ir::log::warn("Save ignored: start buffering first");
        return false;
    }
    if (save_in_progress_.load() || region_picking_.load()) {
        ir::log::warn("Save ignored: export/region pick in progress");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    save_requested_ = true;
    ir::log::info("Save requested (last ~buffer from ring)");
    return true;
}

void CaptureSession::clear_frozen_locked() {
    frozen_.reset();
    freeze_done_ = false;
    freeze_ok_ = false;
    region_freeze_requested_ = false;
}

bool CaptureSession::freeze_region_buffer() {
    if (!ready_.load() || !running_.load()) {
        ir::log::warn("Region freeze ignored: start buffering first");
        return false;
    }
    if (save_in_progress_.load()) {
        ir::log::warn("Region freeze ignored: export still running");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frozen_ && freeze_ok_) {
            region_picking_.store(true);
            return true;
        }
        clear_frozen_locked();
        region_freeze_requested_ = true;
        freeze_done_ = false;
        freeze_ok_ = false;
    }
    ir::log::info("Region freeze requested");

    std::unique_lock<std::mutex> lock(mutex_);
    const bool ok = freeze_cv_.wait_for(lock, std::chrono::seconds(3), [this] {
        return freeze_done_;
    });
    if (!ok || !freeze_ok_ || !frozen_ || frozen_->packets.empty()) {
        ir::log::warn("Region freeze failed or ring empty");
        clear_frozen_locked();
        region_picking_.store(false);
        return false;
    }
    region_picking_.store(true);
    ir::log::info(
        "Region buffer frozen (" + std::to_string(frozen_->packets.size()) +
        " packets) — capture paused for snipping");
    return true;
}

void CaptureSession::cancel_region_freeze() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_frozen_locked();
        freeze_done_ = true;
        freeze_ok_ = false;
    }
    freeze_cv_.notify_all();
    region_picking_.store(false);
    ir::log::info("Region freeze cancelled — buffering resumes");
}

bool CaptureSession::complete_region_save(RECT crop) {
    std::unique_ptr<FrozenClip> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!frozen_ || frozen_->packets.empty()) {
            ir::log::warn("Region complete ignored: nothing frozen");
            region_picking_.store(false);
            return false;
        }
        job = std::move(frozen_);
        clear_frozen_locked();
    }
    region_picking_.store(false);

    const std::string path = ir::paths::make_clip_path();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_clip_path_ = path;
    }
    save_in_progress_.store(true);
    join_save_thread();
    save_thread_ = std::thread([this, path, job = std::move(job), crop]() mutable {
        const ir::export_mp4::ClipAudio* audio_ptr =
            job->have_audio ? &job->audio : nullptr;
        const bool ok = ir::export_mp4::write_region_clip(
            path, job->enc_cfg, job->vextra, job->packets, crop, audio_ptr);
        if (!ok) {
            ir::log::error("Failed to write region clip " + path);
        }
        save_in_progress_.store(false);
    });
    ir::log::info("Region export started (from frozen buffer)");
    return true;
}

int CaptureSession::capture_width() const {
    return duplicator_.width();
}

int CaptureSession::capture_height() const {
    return duplicator_.height();
}

RECT CaptureSession::capture_desktop_rect() const {
    return duplicator_.desktop_rect();
}

CaptureStats CaptureSession::snapshot_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string CaptureSession::last_clip_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_clip_path_;
}

void CaptureSession::audio_thread_main(
    ir::audio::WasapiLoopback* loopback,
    ir::encode::AacEncoder* aac,
    ir::buffer::AudioRingBuffer* audio_ring,
    const ir::encode::AudioEncoderConfig* aac_cfg,
    const ir::time::Timeline* timeline,
    std::atomic<uint64_t>* pcm_frames_total) {
    std::vector<ir::audio::PcmChunk> chunks;
    bool logged_first = false;

    while (running_.load()) {
        chunks.clear();
        const int frames = loopback->read(chunks);
        if (frames > 0 && !region_picking_.load()) {
            if (!logged_first) {
                ir::log::info("WASAPI: receiving audio (" + std::to_string(frames) + " frames/tick)");
                logged_first = true;
            }
            pcm_frames_total->fetch_add(static_cast<uint64_t>(frames));

            for (const auto& chunk : chunks) {
                const int64_t pts = timeline->audio_pts_at(chunk.qpc, aac_cfg->sample_rate);
                std::vector<ir::encode::AudioPacket> apackets;
                if (!aac->encode_interleaved_f32(
                        chunk.interleaved.data(), chunk.frames, pts, apackets)) {
                    ir::log::warn("AAC encode failed for one chunk");
                } else {
                    for (auto& pkt : apackets) {
                        audio_ring->push(std::move(pkt));
                    }
                }
            }
        }
        Sleep(5);
    }

    std::vector<ir::encode::AudioPacket> tail;
    aac->flush(tail);
    for (auto& pkt : tail) {
        audio_ring->push(std::move(pkt));
    }

    const double sec =
        static_cast<double>(audio_ring->duration_samples()) / static_cast<double>(aac_cfg->sample_rate);
    ir::log::info("Audio thread stopped, ring ~" + std::to_string(sec) + "s");
}

void CaptureSession::video_thread_main() {
    if (!duplicator_.init(settings_.monitor_index)) {
        ir::log::error("Capture thread: failed to init Desktop Duplication");
        running_.store(false);
        signal_init(false);
        return;
    }

    ir::encode::EncoderConfig enc_cfg;
    enc_cfg.width = duplicator_.width();
    enc_cfg.height = duplicator_.height();
    enc_cfg.fps = settings_.fps_cap;
    enc_cfg.bitrate =
        settings_.video_bitrate > 0 ? settings_.video_bitrate
                                    : ir::settings::auto_bitrate(settings_.fps_cap);
    enc_cfg.gop_frames = ir::settings::auto_gop_frames(settings_.fps_cap);
    enc_cfg.nvenc_preset = ir::settings::nvenc_preset_for_fps(settings_.fps_cap);

    const ir::hw::VideoCodec resolved = ir::hw::resolve_codec(settings_, hw_);
    enc_cfg.codec = (resolved == ir::hw::VideoCodec::Hevc)
        ? ir::encode::VideoCodecKind::Hevc
        : ir::encode::VideoCodecKind::H264;

    ir::log::info("Capture settings: " + ir::settings::describe(settings_));
    ir::log::info(
        std::string("Resolved video codec: ") + ir::encode::video_codec_label(enc_cfg.codec));

    if (enc_cfg.width != 1920 || enc_cfg.height != 1080) {
        ir::log::warn(
            "Primary monitor is " + std::to_string(enc_cfg.width) + "x" +
            std::to_string(enc_cfg.height) + "; encoding native size");
    }

    ir::encode::NvencEncoder encoder;
    if (!encoder.open(enc_cfg)) {
        if (enc_cfg.codec == ir::encode::VideoCodecKind::Hevc && hw_.nvenc.opens_60) {
            ir::log::warn("HEVC open failed at arm — falling back to H.264");
            enc_cfg.codec = ir::encode::VideoCodecKind::H264;
            if (!encoder.open(enc_cfg)) {
                running_.store(false);
                duplicator_.shutdown();
                signal_init(false);
                return;
            }
        } else {
            running_.store(false);
            duplicator_.shutdown();
            signal_init(false);
            return;
        }
    }

    ir::audio::WasapiLoopback loopback;
    ir::encode::AacEncoder aac;
    ir::encode::AudioEncoderConfig aac_cfg;
    bool audio_ok = false;
    if (loopback.start_preferred(
            settings_.audio_device_id, settings_.audio_device_id.empty())) {
        ir::log::info("System audio device: \"" + loopback.device_name() + "\"");
        aac_cfg.sample_rate = loopback.format().sample_rate;
        aac_cfg.channels = loopback.format().channels;
        aac_cfg.bitrate = 192000;
        audio_ok = aac.open(aac_cfg);
        if (!audio_ok) {
            ir::log::warn("AAC encoder failed — video-only clips");
            loopback.stop();
        }
    } else {
        ir::log::warn("WASAPI loopback failed — video-only clips");
    }

    const int64_t buffer_tb =
        static_cast<int64_t>(enc_cfg.fps) * static_cast<int64_t>(settings_.buffer_sec);
    ir::buffer::EncodedRingBuffer ring(buffer_tb);
    ir::buffer::AudioRingBuffer audio_ring(
        static_cast<int64_t>(aac_cfg.sample_rate) * settings_.buffer_sec);

    // Master clock for A/V — both streams stamp PTS from this QPC origin.
    ir::time::Timeline timeline;
    timeline.reset();
    ir::log::info("A/V timeline: shared QPC master-clock (audio+video PTS from same origin)");

    std::atomic<uint64_t> pcm_frames_total{0};

    if (audio_ok) {
        audio_thread_ = std::thread(
            [this, &loopback, &aac, &audio_ring, &aac_cfg, &timeline, &pcm_frames_total] {
                audio_thread_main(
                    &loopback, &aac, &audio_ring, &aac_cfg, &timeline, &pcm_frames_total);
            });
    }

    signal_init(true);

    CaptureStats local{};
    local.width = enc_cfg.width;
    local.height = enc_cfg.height;

    ir::capture::GpuNv12Converter nv12;
    const bool gpu_nv12 = nv12.init(
        duplicator_.device(), duplicator_.context(), enc_cfg.width, enc_cfg.height);
    if (gpu_nv12) {
        ir::log::info("Encode path: GPU BGRA→NV12 (VideoProcessor) + NVENC");
    } else {
        ir::log::warn("Encode path: CPU BGRA fallback (sws_scale)");
    }

    std::vector<uint8_t> y_plane;
    std::vector<uint8_t> uv_plane;
    if (gpu_nv12) {
        y_plane.resize(static_cast<size_t>(enc_cfg.width) * enc_cfg.height);
        uv_plane.resize(static_cast<size_t>(enc_cfg.width) * enc_cfg.height / 2);
    }

    ID3D11Texture2D* latest_gpu = nullptr;
    bool have_frame = false;
    int64_t last_vpts = -1;
    auto next_frame_deadline = ir::time::now();
    auto window_start = ir::time::now();
    uint64_t frames_at_window_start = 0;
    uint64_t dropped_lag = 0;
    bool logged_gpu_fail = false;

    ir::log::info("Clips folder: " + ir::paths::clips_directory());
    ir::log::info(
        "CFR encode @ " + std::to_string(enc_cfg.fps) +
        " fps (QPC PTS; duplicates last frame if desktop idle; drops slots if NVENC lags)");

    while (running_.load()) {
        // Freeze ring for region pick (content stops advancing for that export).
        bool do_freeze = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (region_freeze_requested_) {
                region_freeze_requested_ = false;
                do_freeze = true;
            }
        }
        if (do_freeze) {
            auto vsnap = ring.snapshot();
            auto frozen = std::make_unique<FrozenClip>();
            frozen->enc_cfg = enc_cfg;
            frozen->vextra = encoder.extradata();
            frozen->packets = std::move(vsnap.packets);
            if (!frozen->packets.empty() && audio_ok) {
                const int64_t v0 = frozen->packets.front().pts;
                const int64_t v1 = frozen->packets.back().pts + 1;
                const double t0 = static_cast<double>(v0) / static_cast<double>(enc_cfg.fps);
                const double t1 = static_cast<double>(v1) / static_cast<double>(enc_cfg.fps);
                const int64_t a0 = static_cast<int64_t>(t0 * aac_cfg.sample_rate + 0.5);
                const int64_t a1 = static_cast<int64_t>(t1 * aac_cfg.sample_rate + 0.5);
                frozen->audio.config = aac_cfg;
                frozen->audio.extradata = aac.extradata();
                auto asnap = audio_ring.snapshot_samples(a0, a1);
                if (asnap.packets.empty()) {
                    asnap = audio_ring.snapshot();
                }
                frozen->audio.packets = std::move(asnap.packets);
                frozen->have_audio = !frozen->audio.packets.empty();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                frozen_ = std::move(frozen);
                freeze_ok_ = frozen_ && !frozen_->packets.empty();
                freeze_done_ = true;
            }
            freeze_cv_.notify_all();
        }

        if (region_picking_.load()) {
            // Keep desktop visually frozen under the snipping overlay.
            Sleep(16);
            continue;
        }

        ID3D11Texture2D* fresh_gpu = nullptr;
        const unsigned timeout = ms_until(next_frame_deadline);
        if (duplicator_.acquire_frame(local, &fresh_gpu, nullptr, timeout)) {
            latest_gpu = fresh_gpu;
            have_frame = true;
        }

        // If we already missed this slot (encode too slow), skip ahead on the schedule.
        // Do NOT invent free-running PTS — wall-clock QPC stamps keep A/V aligned.
        auto now_before = ir::time::now();
        if (now_before > next_frame_deadline + std::chrono::milliseconds(2)) {
            const double late = ir::time::seconds_between(next_frame_deadline, now_before);
            const int skip = static_cast<int>(late * enc_cfg.fps);
            if (skip > 0) {
                dropped_lag += static_cast<uint64_t>(skip);
                next_frame_deadline += std::chrono::duration_cast<ir::time::Clock::duration>(
                    std::chrono::duration<double>(static_cast<double>(skip) / enc_cfg.fps));
            }
        }

        sleep_until(next_frame_deadline);

        if (have_frame && latest_gpu) {
            const int64_t qpc = timeline.qpc_now();
            int64_t pts = timeline.video_pts_at(qpc, enc_cfg.fps);
            if (pts <= last_vpts) {
                pts = last_vpts + 1;
            }

            std::vector<ir::encode::EncodedPacket> packets;
            bool encoded = false;

            if (gpu_nv12) {
                ID3D11Texture2D* nv12_tex = nullptr;
                if (nv12.convert(latest_gpu, &nv12_tex) &&
                    nv12.map_nv12_to_cpu(
                        y_plane.data(), enc_cfg.width, uv_plane.data(), enc_cfg.width)) {
                    encoded = encoder.encode_nv12(
                        y_plane.data(),
                        enc_cfg.width,
                        uv_plane.data(),
                        enc_cfg.width,
                        enc_cfg.width,
                        enc_cfg.height,
                        pts,
                        packets);
                } else if (!logged_gpu_fail) {
                    ir::log::warn("GPU NV12 convert failed once — using CPU BGRA fallback");
                    logged_gpu_fail = true;
                }
            }

            if (!encoded) {
                FrameBuffer cpu;
                if (duplicator_.copy_texture_to_cpu(latest_gpu, cpu)) {
                    encoded = encoder.encode_bgra(
                        cpu.bgra.data(),
                        cpu.width,
                        cpu.height,
                        cpu.stride_bytes,
                        pts,
                        packets);
                }
            }

            if (encoded) {
                for (auto& pkt : packets) {
                    ring.push(std::move(pkt));
                }
                local.frames_encoded++;
                last_vpts = pts;
            } else {
                ir::log::error("encode failed; dropping this frame");
            }
        }

        next_frame_deadline += std::chrono::duration_cast<ir::time::Clock::duration>(
            std::chrono::duration<double>(1.0 / enc_cfg.fps));
        const auto now = ir::time::now();
        // Catch-up if we're more than ~2 frames late.
        if (now > next_frame_deadline + std::chrono::duration_cast<ir::time::Clock::duration>(
                      std::chrono::duration<double>(2.0 / enc_cfg.fps))) {
            next_frame_deadline = now;
        }

        bool do_save = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (save_requested_) {
                save_requested_ = false;
                do_save = true;
            }
        }

        if (do_save) {
            auto vsnap = ring.snapshot();
            if (vsnap.packets.empty()) {
                ir::log::warn("Video ring empty — buffer a few seconds after start");
            } else {
                const std::string path = ir::paths::make_clip_path();
                const auto vextra = encoder.extradata();

                ir::export_mp4::ClipAudio clip_audio;
                clip_audio.config = aac_cfg;
                clip_audio.extradata = aac.extradata();

                const int64_t v0 = vsnap.packets.front().pts;
                const int64_t v1 = vsnap.packets.back().pts + 1;
                const double t0 = static_cast<double>(v0) / static_cast<double>(enc_cfg.fps);
                const double t1 = static_cast<double>(v1) / static_cast<double>(enc_cfg.fps);
                const int64_t a0 = static_cast<int64_t>(t0 * aac_cfg.sample_rate + 0.5);
                const int64_t a1 = static_cast<int64_t>(t1 * aac_cfg.sample_rate + 0.5);

                if (audio_ok) {
                    auto asnap = audio_ring.snapshot_samples(a0, a1);
                    if (asnap.packets.empty()) {
                        asnap = audio_ring.snapshot();
                        ir::log::warn(
                            "Audio trim empty — using full audio ring (" +
                            std::to_string(asnap.packets.size()) + " packets)");
                    }
                    clip_audio.packets = std::move(asnap.packets);
                    ir::log::info(
                        "Clip A/V window: t=[" + std::to_string(t0) + "," + std::to_string(t1) +
                        ")s video_pts=[" + std::to_string(v0) + "," + std::to_string(v1) +
                        ") audio_samples=[" + std::to_string(a0) + "," + std::to_string(a1) +
                        ") packets=" + std::to_string(clip_audio.packets.size()));
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_clip_path_ = path;
                }
                save_in_progress_.store(true);
                join_save_thread();
                const bool have_audio = audio_ok && !clip_audio.packets.empty();
                save_thread_ = std::thread(
                    [this,
                     path,
                     enc_cfg,
                     vextra,
                     packets = std::move(vsnap.packets),
                     clip_audio = std::move(clip_audio),
                     have_audio]() mutable {
                        const ir::export_mp4::ClipAudio* audio_ptr =
                            have_audio ? &clip_audio : nullptr;
                        const bool ok =
                            ir::export_mp4::write_clip(path, enc_cfg, vextra, packets, audio_ptr);
                        if (!ok) {
                            ir::log::error("Failed to write " + path);
                        }
                        save_in_progress_.store(false);
                    });
            }
        }

        const double elapsed = ir::time::seconds_between(window_start, ir::time::now());
        if (elapsed >= 2.0) {
            const uint64_t delta_frames = local.frames_acquired - frames_at_window_start;
            local.updates_per_second = static_cast<double>(delta_frames) / elapsed;
            const double ring_sec = static_cast<double>(ring.duration_tb()) / enc_cfg.fps;
            const double ring_mb = static_cast<double>(ring.bytes()) / (1024.0 * 1024.0);
            const double audio_sec =
                audio_ok ? static_cast<double>(audio_ring.duration_samples()) / aac_cfg.sample_rate
                         : 0.0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_ = local;
            }

            std::ostringstream oss;
            oss << "Capture stats: " << local.width << "x" << local.height
                << " | target=" << enc_cfg.fps << "fps"
                << " | desktop updates/s=" << local.updates_per_second
                << " | encoded=" << local.frames_encoded
                << " | dropped_lag=" << dropped_lag
                << " | last_vpts=" << last_vpts
                << " | video_ring=" << ring_sec << "s (" << ring_mb << " MB)"
                << " | audio_ring=" << audio_sec << "s"
                << " | pcm_frames=" << pcm_frames_total.load()
                << " | timeline=" << timeline.seconds_now() << "s";
            ir::log::info(oss.str());

            window_start = ir::time::now();
            frames_at_window_start = local.frames_acquired;
        }
    }

    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
    if (audio_ok) {
        loopback.stop();
        aac.close();
    }

    std::vector<ir::encode::EncodedPacket> tail;
    encoder.flush(tail);
    for (auto& pkt : tail) {
        ring.push(std::move(pkt));
    }
    encoder.close();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = local;
    }

    duplicator_.shutdown();
    ir::log::info("Capture thread stopped");
}

}  // namespace ir::capture
