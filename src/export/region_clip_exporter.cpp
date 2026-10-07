#include "export/region_clip_exporter.h"

#include "encode/nvenc_encoder.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

namespace ir::export_mp4 {
namespace {

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

RECT sanitize_crop(RECT crop, int full_w, int full_h) {
    crop.left = (std::max)(0L, crop.left) & ~1L;
    crop.top = (std::max)(0L, crop.top) & ~1L;
    crop.right = (std::min)(static_cast<LONG>(full_w), crop.right) & ~1L;
    crop.bottom = (std::min)(static_cast<LONG>(full_h), crop.bottom) & ~1L;
    if (crop.right - crop.left < 32) {
        crop.right = (std::min)(static_cast<LONG>(full_w), crop.left + 32) & ~1L;
    }
    if (crop.bottom - crop.top < 32) {
        crop.bottom = (std::min)(static_cast<LONG>(full_h), crop.top + 32) & ~1L;
    }
    return crop;
}

const AVCodec* find_decoder(ir::encode::VideoCodecKind kind) {
    if (kind == ir::encode::VideoCodecKind::Hevc) {
        const AVCodec* c = avcodec_find_decoder_by_name("hevc");
        return c ? c : avcodec_find_decoder(AV_CODEC_ID_HEVC);
    }
    const AVCodec* c = avcodec_find_decoder_by_name("h264");
    return c ? c : avcodec_find_decoder(AV_CODEC_ID_H264);
}

enum AVPixelFormat get_format_nv12(AVCodecContext*, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_NV12) {
            return AV_PIX_FMT_NV12;
        }
    }
    return pix_fmts[0];
}

bool frame_to_cropped_nv12(
    const AVFrame* src,
    int crop_x,
    int crop_y,
    int crop_w,
    int crop_h,
    std::vector<uint8_t>& y_out,
    std::vector<uint8_t>& uv_out,
    SwsContext** sws_inout) {
    if (!src || !src->data[0]) {
        return false;
    }

    y_out.resize(static_cast<size_t>(crop_w) * static_cast<size_t>(crop_h));
    uv_out.resize(static_cast<size_t>(crop_w) * static_cast<size_t>(crop_h) / 2);

    if (src->format == AV_PIX_FMT_NV12) {
        const uint8_t* sy = src->data[0];
        const uint8_t* suv = src->data[1];
        const int ys = src->linesize[0];
        const int uvs = src->linesize[1];
        for (int row = 0; row < crop_h; ++row) {
            memcpy(
                y_out.data() + static_cast<size_t>(row) * static_cast<size_t>(crop_w),
                sy + (crop_y + row) * ys + crop_x,
                static_cast<size_t>(crop_w));
        }
        for (int row = 0; row < crop_h / 2; ++row) {
            memcpy(
                uv_out.data() + static_cast<size_t>(row) * static_cast<size_t>(crop_w),
                suv + (crop_y / 2 + row) * uvs + crop_x,
                static_cast<size_t>(crop_w));
        }
        return true;
    }

    // Generic path: scale/crop via swscale into NV12.
    *sws_inout = sws_getCachedContext(
        *sws_inout,
        src->width,
        src->height,
        static_cast<AVPixelFormat>(src->format),
        crop_w,
        crop_h,
        AV_PIX_FMT_NV12,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (!*sws_inout) {
        return false;
    }

    // Source region via pointer offsets when possible (YUV420P family).
    const uint8_t* src_data[4] = {
        src->data[0], src->data[1], src->data[2], src->data[3]};
    int src_linesize[4] = {
        src->linesize[0], src->linesize[1], src->linesize[2], src->linesize[3]};

    if (src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P) {
        src_data[0] = src->data[0] + crop_y * src->linesize[0] + crop_x;
        src_data[1] = src->data[1] + (crop_y / 2) * src->linesize[1] + (crop_x / 2);
        src_data[2] = src->data[2] + (crop_y / 2) * src->linesize[2] + (crop_x / 2);
        *sws_inout = sws_getCachedContext(
            *sws_inout,
            crop_w,
            crop_h,
            static_cast<AVPixelFormat>(src->format),
            crop_w,
            crop_h,
            AV_PIX_FMT_NV12,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
    }

    uint8_t* dst_data[4] = {y_out.data(), uv_out.data(), nullptr, nullptr};
    int dst_linesize[4] = {crop_w, crop_w, 0, 0};
    sws_scale(*sws_inout, src_data, src_linesize, 0, crop_h, dst_data, dst_linesize);
    return true;
}

}  // namespace

bool write_region_clip(
    const std::string& path,
    const ir::encode::EncoderConfig& full_config,
    const ir::encode::CodecExtradata& full_extradata,
    const std::vector<ir::encode::EncodedPacket>& full_packets,
    RECT crop,
    const ClipAudio* audio) {
    if (full_packets.empty()) {
        ir::log::warn("Region export skipped: empty video ring");
        return false;
    }

    crop = sanitize_crop(crop, full_config.width, full_config.height);
    const int cw = crop.right - crop.left;
    const int ch = crop.bottom - crop.top;
    if (cw < 32 || ch < 32) {
        ir::log::error("Region crop too small");
        return false;
    }

    ir::log::info(
        "Region export: crop " + std::to_string(crop.left) + "," + std::to_string(crop.top) +
        " " + std::to_string(cw) + "x" + std::to_string(ch) + " from " +
        std::to_string(full_config.width) + "x" + std::to_string(full_config.height));

    const AVCodec* decoder = find_decoder(full_config.codec);
    if (!decoder) {
        ir::log::error("No decoder for region export");
        return false;
    }

    AVCodecContext* dec = avcodec_alloc_context3(decoder);
    if (!dec) {
        return false;
    }
    dec->width = full_config.width;
    dec->height = full_config.height;
    dec->time_base = AVRational{1, full_config.fps};
    dec->pkt_timebase = AVRational{1, full_config.fps};
    dec->framerate = AVRational{full_config.fps, 1};
    dec->get_format = get_format_nv12;
    if (!full_extradata.bytes.empty()) {
        dec->extradata = static_cast<uint8_t*>(
            av_mallocz(full_extradata.bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!dec->extradata) {
            avcodec_free_context(&dec);
            return false;
        }
        memcpy(dec->extradata, full_extradata.bytes.data(), full_extradata.bytes.size());
        dec->extradata_size = static_cast<int>(full_extradata.bytes.size());
    }

    int err = avcodec_open2(dec, decoder, nullptr);
    if (err < 0) {
        ir::log::error("avcodec_open2(decoder) failed: " + av_err_str(err));
        avcodec_free_context(&dec);
        return false;
    }

    ir::encode::EncoderConfig crop_cfg = full_config;
    crop_cfg.width = cw;
    crop_cfg.height = ch;
    const double area_scale = static_cast<double>(cw * ch) /
        static_cast<double>((std::max)(1, full_config.width * full_config.height));
    crop_cfg.bitrate = (std::max)(
        2'000'000,
        static_cast<int>(full_config.bitrate * (std::max)(0.25, area_scale) + 0.5));

    ir::encode::NvencEncoder encoder;
    if (!encoder.open(crop_cfg)) {
        ir::log::error("Region NVENC open failed");
        avcodec_free_context(&dec);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!frame || !pkt) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        return false;
    }

    std::vector<ir::encode::EncodedPacket> out_packets;
    out_packets.reserve(full_packets.size());
    std::vector<uint8_t> y_plane;
    std::vector<uint8_t> uv_plane;
    SwsContext* sws = nullptr;
    // Keep the capture timeline (same clock as audio). Remapping to 0..N-1 collapses
    // dropped-frame gaps and makes video look sped up vs WASAPI/AAC.
    const int64_t origin_pts =
        full_packets.front().pts == AV_NOPTS_VALUE ? 0 : full_packets.front().pts;
    int64_t last_frame_pts = AV_NOPTS_VALUE;
    int decoded_frames = 0;

    auto drain_decoder = [&]() -> bool {
        for (;;) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return true;
            }
            if (r < 0) {
                ir::log::error("avcodec_receive_frame failed: " + av_err_str(r));
                return false;
            }

            int64_t frame_pts = frame->best_effort_timestamp;
            if (frame_pts == AV_NOPTS_VALUE) {
                frame_pts = frame->pts;
            }
            if (frame_pts == AV_NOPTS_VALUE) {
                frame_pts =
                    (last_frame_pts == AV_NOPTS_VALUE) ? origin_pts : (last_frame_pts + 1);
            }
            last_frame_pts = frame_pts;

            if (!frame_to_cropped_nv12(
                    frame, crop.left, crop.top, cw, ch, y_plane, uv_plane, &sws)) {
                // Keep timeline continuous even if one convert fails.
                y_plane.assign(static_cast<size_t>(cw) * static_cast<size_t>(ch), 0);
                uv_plane.assign(static_cast<size_t>(cw) * static_cast<size_t>(ch) / 2, 128);
            }
            std::vector<ir::encode::EncodedPacket> encoded;
            if (!encoder.encode_nv12(
                    y_plane.data(), cw, uv_plane.data(), cw, cw, ch, frame_pts, encoded)) {
                ir::log::warn("Region encode_nv12 failed for one frame");
            } else {
                for (auto& ep : encoded) {
                    if (ep.duration <= 0) {
                        ep.duration = 1;
                    }
                    out_packets.push_back(std::move(ep));
                }
            }
            ++decoded_frames;
            av_frame_unref(frame);
        }
    };

    for (const auto& src : full_packets) {
        av_packet_unref(pkt);
        err = av_new_packet(pkt, static_cast<int>(src.data.size()));
        if (err < 0) {
            continue;
        }
        memcpy(pkt->data, src.data.data(), src.data.size());
        pkt->pts = src.pts;
        pkt->dts = src.dts == AV_NOPTS_VALUE ? src.pts : src.dts;
        pkt->flags = src.is_keyframe ? AV_PKT_FLAG_KEY : 0;
        err = avcodec_send_packet(dec, pkt);
        if (err < 0) {
            ir::log::warn("avcodec_send_packet failed: " + av_err_str(err));
            continue;
        }
        if (!drain_decoder()) {
            if (sws) {
                sws_freeContext(sws);
            }
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&dec);
            return false;
        }
    }

    avcodec_send_packet(dec, nullptr);
    drain_decoder();

    std::vector<ir::encode::EncodedPacket> tail;
    encoder.flush(tail);
    for (auto& ep : tail) {
        if (ep.duration <= 0) {
            ep.duration = 1;
        }
        out_packets.push_back(std::move(ep));
    }

    const ir::encode::CodecExtradata crop_extra = encoder.extradata();
    encoder.close();

    if (sws) {
        sws_freeContext(sws);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec);

    if (out_packets.empty()) {
        ir::log::error(
            "Region export produced no packets (decoded_frames=" +
            std::to_string(decoded_frames) + ")");
        return false;
    }

    ir::log::info(
        "Region re-encoded " + std::to_string(decoded_frames) + " frames → " +
        std::to_string(out_packets.size()) + " packets");

    // Align audio to the re-encoded video clock. write_clip subtracts first video PTS;
    // if decode/encode shifted the video origin (e.g. to 0), shift audio by the same delta
    // and trim to the video span so A/V stay locked.
    ClipAudio remapped_audio;
    const ClipAudio* audio_out = audio;
    if (audio && !audio->packets.empty()) {
        int64_t dst_v0 = out_packets.front().pts;
        int64_t dst_v1 = out_packets.front().pts;
        for (const auto& p : out_packets) {
            if (p.pts == AV_NOPTS_VALUE) {
                continue;
            }
            dst_v0 = (std::min)(dst_v0, p.pts);
            dst_v1 = (std::max)(dst_v1, p.pts);
        }

        const AVRational vtb{1, crop_cfg.fps};
        const AVRational atb{1, audio->config.sample_rate};
        const int64_t src_origin_a = av_rescale_q(origin_pts, vtb, atb);
        const int64_t dst_origin_a = av_rescale_q(dst_v0, vtb, atb);
        const int64_t dst_end_a = av_rescale_q(dst_v1 + 1, vtb, atb);
        const int64_t shift = dst_origin_a - src_origin_a;

        remapped_audio.config = audio->config;
        remapped_audio.extradata = audio->extradata;
        remapped_audio.packets.reserve(audio->packets.size());
        for (const auto& ap : audio->packets) {
            ir::encode::AudioPacket copy = ap;
            copy.pts = ap.pts + shift;
            if (copy.pts + copy.duration <= dst_origin_a) {
                continue;
            }
            if (copy.pts >= dst_end_a) {
                continue;
            }
            remapped_audio.packets.push_back(std::move(copy));
        }
        audio_out = remapped_audio.packets.empty() ? nullptr : &remapped_audio;
        ir::log::info(
            "Region audio remap: shift=" + std::to_string(shift) + " packets=" +
            std::to_string(remapped_audio.packets.size()) + "/" +
            std::to_string(audio->packets.size()));
    }

    return write_clip(path, crop_cfg, crop_extra, out_packets, audio_out);
}

}  // namespace ir::export_mp4
