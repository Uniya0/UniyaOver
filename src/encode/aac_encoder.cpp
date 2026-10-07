#include "encode/aac_encoder.h"

#include "util/log.h"

#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace ir::encode {
namespace {

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

struct AacEncoder::Impl {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
};

AacEncoder::~AacEncoder() {
    close();
}

bool AacEncoder::open(const AudioEncoderConfig& config) {
    close();
    impl_ = new Impl();
    config_ = config;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        ir::log::error("AAC encoder not found in FFmpeg build");
        cleanup();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(codec);
    if (!impl_->codec) {
        ir::log::error("avcodec_alloc_context3(AAC) failed");
        cleanup();
        return false;
    }

    impl_->codec->sample_rate = config.sample_rate;
    impl_->codec->bit_rate = config.bitrate;
    impl_->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&impl_->codec->ch_layout, config.channels);
    impl_->codec->time_base = AVRational{1, config.sample_rate};
    impl_->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int err = avcodec_open2(impl_->codec, codec, nullptr);
    if (err < 0) {
        ir::log::error("avcodec_open2(AAC) failed: " + av_err_str(err));
        cleanup();
        return false;
    }

    frame_samples_ = impl_->codec->frame_size > 0 ? impl_->codec->frame_size : 1024;
    encoder_delay_ = impl_->codec->initial_padding > 0 ? impl_->codec->initial_padding : 0;

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (!impl_->frame || !impl_->packet) {
        ir::log::error("AAC frame/packet alloc failed");
        cleanup();
        return false;
    }

    impl_->frame->format = impl_->codec->sample_fmt;
    impl_->frame->sample_rate = impl_->codec->sample_rate;
    impl_->frame->nb_samples = frame_samples_;
    av_channel_layout_copy(&impl_->frame->ch_layout, &impl_->codec->ch_layout);
    err = av_frame_get_buffer(impl_->frame, 0);
    if (err < 0) {
        ir::log::error("av_frame_get_buffer(AAC) failed: " + av_err_str(err));
        cleanup();
        return false;
    }

    if (impl_->codec->extradata && impl_->codec->extradata_size > 0) {
        extradata_.bytes.assign(
            impl_->codec->extradata,
            impl_->codec->extradata + impl_->codec->extradata_size);
    }

    pending_.clear();
    pending_pts_ = 0;
    have_pending_pts_ = false;
    opened_ = true;
    ir::log::info(
        "AAC encoder ready: " + std::to_string(config.channels) + "ch @" +
        std::to_string(config.sample_rate) + " Hz, " + std::to_string(config.bitrate / 1000) +
        " kbps, frame=" + std::to_string(frame_samples_) + " samples, delay=" +
        std::to_string(encoder_delay_) + " samples");
    return true;
}

void AacEncoder::close() {
    opened_ = false;
    cleanup();
}

bool AacEncoder::encode_interleaved_f32(
    const float* interleaved,
    int sample_count,
    int64_t first_sample_pts,
    std::vector<AudioPacket>& out) {
    if (!opened_ || !impl_ || sample_count <= 0 || !interleaved) {
        return sample_count == 0;
    }

    const int ch = config_.channels;
    if (!have_pending_pts_ || pending_.empty()) {
        pending_pts_ = first_sample_pts;
        have_pending_pts_ = true;
    }
    pending_.insert(
        pending_.end(), interleaved, interleaved + static_cast<size_t>(sample_count) * ch);
    return encode_full_frames(out);
}

bool AacEncoder::encode_full_frames(std::vector<AudioPacket>& out) {
    const int ch = config_.channels;
    const size_t need = static_cast<size_t>(frame_samples_) * static_cast<size_t>(ch);

    while (pending_.size() >= need) {
        int err = av_frame_make_writable(impl_->frame);
        if (err < 0) {
            ir::log::error("av_frame_make_writable(AAC) failed: " + av_err_str(err));
            return false;
        }

        for (int c = 0; c < ch; ++c) {
            float* dst = reinterpret_cast<float*>(impl_->frame->data[c]);
            for (int i = 0; i < frame_samples_; ++i) {
                dst[i] = pending_[static_cast<size_t>(i) * ch + c];
            }
        }

        impl_->frame->pts = pending_pts_;
        pending_pts_ += frame_samples_;

        err = avcodec_send_frame(impl_->codec, impl_->frame);
        if (err < 0) {
            ir::log::error("avcodec_send_frame(AAC) failed: " + av_err_str(err));
            return false;
        }
        if (!drain_packets(out)) {
            return false;
        }

        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(need));
    }
    return true;
}

bool AacEncoder::flush(std::vector<AudioPacket>& out) {
    if (!opened_ || !impl_) {
        return true;
    }

    // Pad leftover samples with silence so the last AAC frame is emitted.
    if (!pending_.empty()) {
        const size_t need = static_cast<size_t>(frame_samples_) * config_.channels;
        pending_.resize(need, 0.0f);
        if (!encode_full_frames(out)) {
            return false;
        }
    }

    const int err = avcodec_send_frame(impl_->codec, nullptr);
    if (err < 0 && err != AVERROR_EOF) {
        ir::log::error("avcodec_send_frame(AAC flush) failed: " + av_err_str(err));
        return false;
    }
    return drain_packets(out);
}

bool AacEncoder::drain_packets(std::vector<AudioPacket>& out) {
    while (true) {
        const int err = avcodec_receive_packet(impl_->codec, impl_->packet);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            break;
        }
        if (err < 0) {
            ir::log::error("avcodec_receive_packet(AAC) failed: " + av_err_str(err));
            return false;
        }

        AudioPacket pkt;
        pkt.data.assign(impl_->packet->data, impl_->packet->data + impl_->packet->size);
        pkt.pts = impl_->packet->pts;
        if (pkt.pts != AV_NOPTS_VALUE && encoder_delay_ > 0) {
            pkt.pts -= encoder_delay_;
        }
        pkt.duration = impl_->packet->duration > 0 ? impl_->packet->duration : frame_samples_;
        out.push_back(std::move(pkt));
        av_packet_unref(impl_->packet);
    }
    return true;
}

void AacEncoder::cleanup() {
    pending_.clear();
    pending_pts_ = 0;
    have_pending_pts_ = false;
    frame_samples_ = 1024;
    encoder_delay_ = 0;
    extradata_.bytes.clear();
    if (!impl_) {
        opened_ = false;
        return;
    }
    if (impl_->packet) {
        av_packet_free(&impl_->packet);
    }
    if (impl_->frame) {
        av_frame_free(&impl_->frame);
    }
    if (impl_->codec) {
        avcodec_free_context(&impl_->codec);
    }
    delete impl_;
    impl_ = nullptr;
    opened_ = false;
}

}  // namespace ir::encode
