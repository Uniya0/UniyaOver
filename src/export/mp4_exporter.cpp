#include "export/mp4_exporter.h"

#include "util/log.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
}

namespace ir::export_mp4 {
namespace {

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

bool write_one_video(
    AVFormatContext* format,
    AVStream* vstream,
    const AVRational& video_tb,
    const ir::encode::EncodedPacket& src,
    int64_t first_vpts,
    int64_t fallback_pts) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        return false;
    }
    int err = av_new_packet(pkt, static_cast<int>(src.data.size()));
    if (err < 0) {
        av_packet_free(&pkt);
        return false;
    }
    memcpy(pkt->data, src.data.data(), src.data.size());

    const int64_t pts = src.pts == AV_NOPTS_VALUE ? fallback_pts : (src.pts - first_vpts);
    int64_t dts = src.dts;
    if (dts == AV_NOPTS_VALUE) {
        dts = pts;
    } else {
        dts -= first_vpts;
    }

    pkt->pts = pts;
    pkt->dts = dts;
    pkt->duration = src.duration > 0 ? src.duration : 1;
    pkt->stream_index = vstream->index;
    pkt->flags = src.is_keyframe ? AV_PKT_FLAG_KEY : 0;
    av_packet_rescale_ts(pkt, video_tb, vstream->time_base);

    err = av_interleaved_write_frame(format, pkt);
    av_packet_free(&pkt);
    return err >= 0;
}

bool write_one_audio(
    AVFormatContext* format,
    AVStream* astream,
    const AVRational& audio_tb,
    const ir::encode::AudioPacket& src,
    int64_t video_origin_samples) {
    const int64_t pts = src.pts - video_origin_samples;
    if (pts + src.duration <= 0) {
        return true;  // skip, not an error
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        return false;
    }
    int err = av_new_packet(pkt, static_cast<int>(src.data.size()));
    if (err < 0) {
        av_packet_free(&pkt);
        return false;
    }
    memcpy(pkt->data, src.data.data(), src.data.size());

    pkt->pts = pts < 0 ? 0 : pts;
    pkt->dts = pkt->pts;
    pkt->duration = src.duration > 0 ? src.duration : 1024;
    pkt->stream_index = astream->index;
    av_packet_rescale_ts(pkt, audio_tb, astream->time_base);

    err = av_interleaved_write_frame(format, pkt);
    av_packet_free(&pkt);
    return err >= 0;
}

}  // namespace

bool write_clip(
    const std::string& path,
    const ir::encode::EncoderConfig& video_config,
    const ir::encode::CodecExtradata& video_extradata,
    const std::vector<ir::encode::EncodedPacket>& video_packets,
    const ClipAudio* audio) {
    if (video_packets.empty()) {
        ir::log::warn("MP4 export skipped: video ring is empty");
        return false;
    }

    AVFormatContext* format = nullptr;
    int err = avformat_alloc_output_context2(&format, nullptr, "mp4", path.c_str());
    if (err < 0 || !format) {
        ir::log::error("avformat_alloc_output_context2 failed: " + av_err_str(err));
        return false;
    }

    AVStream* vstream = avformat_new_stream(format, nullptr);
    if (!vstream) {
        ir::log::error("avformat_new_stream(video) failed");
        avformat_free_context(format);
        return false;
    }

    const AVRational video_tb{1, video_config.fps};
    vstream->time_base = video_tb;
    vstream->avg_frame_rate = AVRational{video_config.fps, 1};
    vstream->r_frame_rate = AVRational{video_config.fps, 1};

    AVCodecParameters* vpar = vstream->codecpar;
    vpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vpar->codec_id = video_config.codec == ir::encode::VideoCodecKind::Hevc
        ? AV_CODEC_ID_HEVC
        : AV_CODEC_ID_H264;
    vpar->width = video_config.width;
    vpar->height = video_config.height;
    vpar->format = AV_PIX_FMT_NV12;
    vpar->bit_rate = video_config.bitrate;
    vpar->color_range = AVCOL_RANGE_MPEG;
    vpar->color_space = AVCOL_SPC_BT709;
    vpar->color_primaries = AVCOL_PRI_BT709;
    vpar->color_trc = AVCOL_TRC_BT709;

    if (!video_extradata.bytes.empty()) {
        vpar->extradata = static_cast<uint8_t*>(
            av_mallocz(video_extradata.bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!vpar->extradata) {
            ir::log::error("av_mallocz(video extradata) failed");
            avformat_free_context(format);
            return false;
        }
        memcpy(vpar->extradata, video_extradata.bytes.data(), video_extradata.bytes.size());
        vpar->extradata_size = static_cast<int>(video_extradata.bytes.size());
    }

    AVStream* astream = nullptr;
    AVRational audio_tb{1, 48000};
    const bool have_audio = audio && !audio->packets.empty();
    if (have_audio) {
        astream = avformat_new_stream(format, nullptr);
        if (!astream) {
            ir::log::error("avformat_new_stream(audio) failed");
            avformat_free_context(format);
            return false;
        }

        audio_tb = AVRational{1, audio->config.sample_rate};
        astream->time_base = audio_tb;

        AVCodecParameters* apar = astream->codecpar;
        apar->codec_type = AVMEDIA_TYPE_AUDIO;
        apar->codec_id = AV_CODEC_ID_AAC;
        apar->sample_rate = audio->config.sample_rate;
        apar->bit_rate = audio->config.bitrate;
        apar->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&apar->ch_layout, audio->config.channels);
        if (!audio->extradata.bytes.empty()) {
            apar->extradata = static_cast<uint8_t*>(
                av_mallocz(audio->extradata.bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!apar->extradata) {
                ir::log::error("av_mallocz(audio extradata) failed");
                avformat_free_context(format);
                return false;
            }
            memcpy(apar->extradata, audio->extradata.bytes.data(), audio->extradata.bytes.size());
            apar->extradata_size = static_cast<int>(audio->extradata.bytes.size());
        } else {
            ir::log::warn("AAC extradata missing — some players will have no audio");
        }
    }

    if (!(format->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            ir::log::error("avio_open failed: " + av_err_str(err) + " path=" + path);
            avformat_free_context(format);
            return false;
        }
    }

    err = avformat_write_header(format, nullptr);
    if (err < 0) {
        ir::log::error("avformat_write_header failed: " + av_err_str(err));
        if (!(format->oformat->flags & AVFMT_NOFILE) && format->pb) {
            avio_closep(&format->pb);
        }
        avformat_free_context(format);
        return false;
    }

    const int64_t first_vpts = video_packets.front().pts;
    const int64_t video_origin_samples =
        have_audio ? av_rescale_q(first_vpts, video_tb, audio_tb) : 0;

    // Interleave by timeline so A/V stay aligned in the file (not all video then audio).
    size_t vi = 0;
    size_t ai = 0;
    int64_t written_v = 0;
    int64_t written_a = 0;
    bool ok = true;

    while (ok && (vi < video_packets.size() || (have_audio && ai < audio->packets.size()))) {
        bool take_video = false;
        if (vi >= video_packets.size()) {
            take_video = false;
        } else if (!have_audio || ai >= audio->packets.size()) {
            take_video = true;
        } else {
            const int64_t v_pts = video_packets[vi].pts == AV_NOPTS_VALUE
                ? static_cast<int64_t>(vi)
                : (video_packets[vi].pts - first_vpts);
            const int64_t v_in_audio_tb = av_rescale_q(v_pts, video_tb, audio_tb);
            const int64_t a_pts = audio->packets[ai].pts - video_origin_samples;
            take_video = v_in_audio_tb <= a_pts;
        }

        if (take_video) {
            if (!write_one_video(
                    format, vstream, video_tb, video_packets[vi], first_vpts, written_v)) {
                ir::log::error("write video packet failed");
                ok = false;
                break;
            }
            ++vi;
            ++written_v;
        } else {
            if (!write_one_audio(
                    format, astream, audio_tb, audio->packets[ai], video_origin_samples)) {
                ir::log::error("write audio packet failed");
                ok = false;
                break;
            }
            ++ai;
            ++written_a;
        }
    }

    av_write_trailer(format);
    if (!(format->oformat->flags & AVFMT_NOFILE) && format->pb) {
        avio_closep(&format->pb);
    }
    avformat_free_context(format);

    if (!ok || written_v == 0) {
        return false;
    }

    const double seconds = static_cast<double>(written_v) / video_config.fps;
    ir::log::info(
        "Saved MP4: " + path + " video_packets=" + std::to_string(written_v) +
        " audio_packets=" + std::to_string(written_a) + " (~" + std::to_string(seconds) + "s)");
    return true;
}

}  // namespace ir::export_mp4
