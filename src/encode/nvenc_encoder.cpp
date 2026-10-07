#include "encode/nvenc_encoder.h"

#include "util/log.h"

#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace ir::encode {
namespace {

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

struct NvencEncoder::Impl {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int src_w = 0;
    int src_h = 0;
};

NvencEncoder::~NvencEncoder() {
    close();
}

bool NvencEncoder::open(const EncoderConfig& config) {
    close();

    impl_ = new Impl();
    config_ = config;

    const char* encoder_name = nvenc_encoder_name(config.codec);
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        ir::log::error(std::string(encoder_name) + " not found. Need an FFmpeg build with NVENC.");
        cleanup();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(codec);
    if (!impl_->codec) {
        ir::log::error("avcodec_alloc_context3 failed");
        cleanup();
        return false;
    }

    impl_->codec->codec_id = codec->id;
    impl_->codec->width = config.width;
    impl_->codec->height = config.height;
    impl_->codec->time_base = AVRational{1, config.fps};
    impl_->codec->framerate = AVRational{config.fps, 1};
    impl_->codec->pix_fmt = AV_PIX_FMT_NV12;
    impl_->codec->bit_rate = config.bitrate;
    impl_->codec->gop_size = config.gop_frames;
    impl_->codec->max_b_frames = 0;
    // Put codec headers in extradata so every saved MP4 can start on a later GOP.
    impl_->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(impl_->codec->priv_data, "preset", config.nvenc_preset ? config.nvenc_preset : "p5", 0);
    av_opt_set(impl_->codec->priv_data, "tune", "ll", 0);    // low latency
    av_opt_set(impl_->codec->priv_data, "rc", "vbr", 0);
    const int cq = config.fps >= 144 ? 21 : 19;
    av_opt_set_int(impl_->codec->priv_data, "cq", cq, 0);
    impl_->codec->rc_max_rate = config.bitrate;
    impl_->codec->rc_buffer_size = config.bitrate * 2;

    int err = avcodec_open2(impl_->codec, codec, nullptr);
    if (err < 0) {
        ir::log::error(
            std::string("avcodec_open2(") + encoder_name + ") failed: " + av_err_str(err));
        ir::log::error(
            "NVENC open failed. Usual cause: FFmpeg was built for a newer nvenc API "
            "than your NVIDIA driver exposes.");
        ir::log::error(
            "Fix: update GeForce driver (Game Ready / Studio), OR replace "
            "third_party/ffmpeg with an older BtbN win64-gpl-shared build.");
        cleanup();
        return false;
    }

    if (impl_->codec->extradata && impl_->codec->extradata_size > 0) {
        extradata_.bytes.assign(
            impl_->codec->extradata, impl_->codec->extradata + impl_->codec->extradata_size);
    } else {
        ir::log::warn("NVENC opened without extradata; MP4 players may fail to init");
    }

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (!impl_->frame || !impl_->packet) {
        ir::log::error("av_frame_alloc/av_packet_alloc failed");
        cleanup();
        return false;
    }

    impl_->frame->format = impl_->codec->pix_fmt;
    impl_->frame->width = impl_->codec->width;
    impl_->frame->height = impl_->codec->height;
    err = av_frame_get_buffer(impl_->frame, 32);
    if (err < 0) {
        ir::log::error("av_frame_get_buffer failed: " + av_err_str(err));
        cleanup();
        return false;
    }

    opened_ = true;
    ir::log::info(
        std::string("NVENC encoder ready: ") + video_codec_label(config.codec) + " " +
        std::to_string(config.width) + "x" + std::to_string(config.height) + " @ " +
        std::to_string(config.fps) + " fps, " + std::to_string(config.bitrate / 1000) +
        " kbps, gop=" + std::to_string(config.gop_frames) +
        ", preset=" + std::string(config.nvenc_preset ? config.nvenc_preset : "p5") +
        " (no B-frames)");
    return true;
}

void NvencEncoder::close() {
    opened_ = false;
    cleanup();
}

bool NvencEncoder::encode_nv12(
    const uint8_t* y,
    int y_stride,
    const uint8_t* uv,
    int uv_stride,
    int width,
    int height,
    int64_t pts,
    std::vector<EncodedPacket>& out) {
    if (!opened_ || !impl_ || !y || !uv) {
        return false;
    }
    if (width != config_.width || height != config_.height) {
        return false;
    }
    if (y_stride < width || uv_stride < width) {
        return false;
    }

    int err = av_frame_make_writable(impl_->frame);
    if (err < 0) {
        ir::log::error("av_frame_make_writable failed: " + av_err_str(err));
        return false;
    }

    for (int row = 0; row < height; ++row) {
        std::memcpy(
            impl_->frame->data[0] + row * impl_->frame->linesize[0],
            y + row * y_stride,
            static_cast<size_t>(width));
    }
    for (int row = 0; row < height / 2; ++row) {
        std::memcpy(
            impl_->frame->data[1] + row * impl_->frame->linesize[1],
            uv + row * uv_stride,
            static_cast<size_t>(width));
    }

    impl_->frame->pts = pts;
    err = avcodec_send_frame(impl_->codec, impl_->frame);
    if (err < 0) {
        ir::log::error("avcodec_send_frame failed: " + av_err_str(err));
        return false;
    }
    return drain_packets(out);
}

bool NvencEncoder::encode_bgra(
    const uint8_t* bgra,
    int width,
    int height,
    int stride_bytes,
    int64_t pts,
    std::vector<EncodedPacket>& out) {
    if (!opened_ || !impl_ || !bgra) {
        return false;
    }

    if (width != config_.width || height != config_.height) {
        ir::log::error(
            "Frame size mismatch: got " + std::to_string(width) + "x" + std::to_string(height) +
            ", expected " + std::to_string(config_.width) + "x" + std::to_string(config_.height));
        return false;
    }

    if (stride_bytes < width * 4) {
        ir::log::error("BGRA stride too small");
        return false;
    }

    if (!impl_->sws || impl_->src_w != width || impl_->src_h != height) {
        if (impl_->sws) {
            sws_freeContext(impl_->sws);
            impl_->sws = nullptr;
        }
        impl_->sws = sws_getContext(
            width,
            height,
            AV_PIX_FMT_BGRA,
            config_.width,
            config_.height,
            AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!impl_->sws) {
            ir::log::error("sws_getContext failed");
            return false;
        }
        impl_->src_w = width;
        impl_->src_h = height;
    }

    int err = av_frame_make_writable(impl_->frame);
    if (err < 0) {
        ir::log::error("av_frame_make_writable failed: " + av_err_str(err));
        return false;
    }

    const uint8_t* src_slices[4] = {bgra, nullptr, nullptr, nullptr};
    int src_stride[4] = {stride_bytes, 0, 0, 0};
    sws_scale(
        impl_->sws, src_slices, src_stride, 0, height, impl_->frame->data, impl_->frame->linesize);

    impl_->frame->pts = pts;

    err = avcodec_send_frame(impl_->codec, impl_->frame);
    if (err < 0) {
        ir::log::error("avcodec_send_frame failed: " + av_err_str(err));
        return false;
    }

    return drain_packets(out);
}

bool NvencEncoder::flush(std::vector<EncodedPacket>& out) {
    if (!opened_ || !impl_) {
        return true;
    }
    const int err = avcodec_send_frame(impl_->codec, nullptr);
    if (err < 0 && err != AVERROR_EOF) {
        ir::log::error("avcodec_send_frame(flush) failed: " + av_err_str(err));
        return false;
    }
    return drain_packets(out);
}

bool NvencEncoder::drain_packets(std::vector<EncodedPacket>& out) {
    while (true) {
        const int err = avcodec_receive_packet(impl_->codec, impl_->packet);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            break;
        }
        if (err < 0) {
            ir::log::error("avcodec_receive_packet failed: " + av_err_str(err));
            return false;
        }

        EncodedPacket pkt;
        pkt.data.assign(impl_->packet->data, impl_->packet->data + impl_->packet->size);
        pkt.pts = impl_->packet->pts;
        pkt.dts = impl_->packet->dts;
        pkt.duration = impl_->packet->duration > 0 ? impl_->packet->duration : 1;
        pkt.is_keyframe = (impl_->packet->flags & AV_PKT_FLAG_KEY) != 0;
        out.push_back(std::move(pkt));

        av_packet_unref(impl_->packet);
    }
    return true;
}

void NvencEncoder::cleanup() {
    if (!impl_) {
        opened_ = false;
        extradata_.bytes.clear();
        return;
    }

    if (impl_->sws) {
        sws_freeContext(impl_->sws);
        impl_->sws = nullptr;
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
    extradata_.bytes.clear();
}

}  // namespace ir::encode
