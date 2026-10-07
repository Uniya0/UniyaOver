#include "gui/clip_thumbnail.h"

#include "util/log.h"
#include "util/paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

namespace ir::gui {
namespace {

std::string av_err_str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

bool file_stamp(const std::wstring& path, uint64_t* size_out, uint64_t* mtime_out) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    *size_out = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    ULARGE_INTEGER u{};
    u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    *mtime_out = u.QuadPart;
    return true;
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

int get_encoder_clsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) {
        return -1;
    }
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return 0;
        }
    }
    return -1;
}

}  // namespace

std::wstring thumbs_directory() {
    const std::wstring dir = utf8_to_wide(ir::paths::app_data_directory()) + L"\\thumbs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring thumbnail_cache_path(const std::wstring& mp4_path, int width, int height) {
    uint64_t size = 0;
    uint64_t mtime = 0;
    if (!file_stamp(mp4_path, &size, &mtime)) {
        return {};
    }
    const uint64_t path_hash = fnv1a64(wide_to_utf8(mp4_path));
    wchar_t name[160];
    std::swprintf(
        name,
        160,
        L"%016llx_%llu_%llu_%dx%d.jpg",
        static_cast<unsigned long long>(path_hash),
        static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(mtime),
        width,
        height);
    return thumbs_directory() + L"\\" + name;
}

bool thumbnail_cache_valid(const std::wstring& mp4_path, int width, int height) {
    const std::wstring cache = thumbnail_cache_path(mp4_path, width, height);
    if (cache.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(cache.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool decode_clip_thumbnail_bgra(
    const std::wstring& mp4_path,
    int width,
    int height,
    ThumbnailPixels& out) {
    out = {};
    const std::string path_utf8 = wide_to_utf8(mp4_path);
    if (path_utf8.empty() || width < 16 || height < 16) {
        return false;
    }

    AVFormatContext* fmt = nullptr;
    int err = avformat_open_input(&fmt, path_utf8.c_str(), nullptr, nullptr);
    if (err < 0) {
        ir::log::warn("thumb open failed: " + av_err_str(err));
        return false;
    }
    err = avformat_find_stream_info(fmt, nullptr);
    if (err < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    const int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vindex < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    AVStream* st = fmt->streams[vindex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec) {
        avformat_close_input(&fmt);
        return false;
    }
    avcodec_parameters_to_context(dec, st->codecpar);
    err = avcodec_open2(dec, codec, nullptr);
    if (err < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    if (st->duration > 0) {
        av_seek_frame(fmt, vindex, st->duration / 7, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
    } else if (fmt->duration > 0) {
        const int64_t target =
            av_rescale_q(fmt->duration / 7, AVRational{1, AV_TIME_BASE}, st->time_base);
        av_seek_frame(fmt, vindex, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    bool got_frame = false;
    auto try_read = [&]() {
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index != vindex) {
                av_packet_unref(pkt);
                continue;
            }
            err = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (err < 0) {
                continue;
            }
            err = avcodec_receive_frame(dec, frame);
            if (err == 0) {
                got_frame = true;
                return;
            }
        }
    };

    try_read();
    if (!got_frame) {
        av_seek_frame(fmt, vindex, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        try_read();
    }

    bool ok = false;
    if (got_frame && frame->width > 0 && frame->height > 0) {
        const int src_w = frame->width;
        const int src_h = frame->height;
        // Cover-fit into the target rectangle (16:9 thumbs, etc.).
        const double scale = (std::max)(
            static_cast<double>(width) / src_w,
            static_cast<double>(height) / src_h);
        const int scaled_w = (std::max)(1, static_cast<int>(src_w * scale + 0.5));
        const int scaled_h = (std::max)(1, static_cast<int>(src_h * scale + 0.5));

        std::vector<uint8_t> scaled(
            static_cast<size_t>(scaled_w) * static_cast<size_t>(scaled_h) * 4);
        uint8_t* dst_slices[4] = {scaled.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {scaled_w * 4, 0, 0, 0};

        SwsContext* sws = sws_getContext(
            src_w,
            src_h,
            static_cast<AVPixelFormat>(frame->format),
            scaled_w,
            scaled_h,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (sws) {
            sws_scale(
                sws, frame->data, frame->linesize, 0, src_h, dst_slices, dst_stride);
            sws_freeContext(sws);

            const int ox = (scaled_w - width) / 2;
            const int oy = (scaled_h - height) / 2;
            out.width = width;
            out.height = height;
            out.bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
            for (int y = 0; y < height; ++y) {
                memcpy(
                    out.bgra.data() + static_cast<size_t>(y) * width * 4,
                    scaled.data() +
                        (static_cast<size_t>(oy + y) * scaled_w + static_cast<size_t>(ox)) * 4,
                    static_cast<size_t>(width) * 4);
            }
            ok = true;
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ok;
}

Gdiplus::Bitmap* bitmap_from_bgra(const ThumbnailPixels& pixels) {
    if (pixels.width <= 0 || pixels.height <= 0 ||
        pixels.bgra.size() <
            static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height) * 4) {
        return nullptr;
    }
    auto* bmp = new Gdiplus::Bitmap(pixels.width, pixels.height, PixelFormat32bppARGB);
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, pixels.width, pixels.height);
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) !=
        Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    const int src_stride = pixels.width * 4;
    for (int y = 0; y < pixels.height; ++y) {
        memcpy(
            static_cast<uint8_t*>(data.Scan0) + y * data.Stride,
            pixels.bgra.data() + static_cast<size_t>(y) * src_stride,
            static_cast<size_t>(src_stride));
    }
    bmp->UnlockBits(&data);
    return bmp;
}

Gdiplus::Bitmap* load_thumbnail_bitmap_from_cache(
    const std::wstring& mp4_path,
    int width,
    int height) {
    const std::wstring cache = thumbnail_cache_path(mp4_path, width, height);
    if (cache.empty()) {
        return nullptr;
    }
    auto* bmp = new Gdiplus::Bitmap(cache.c_str());
    if (bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0) {
        return bmp;
    }
    delete bmp;
    return nullptr;
}

bool save_thumbnail_jpeg(
    const ThumbnailPixels& pixels,
    const std::wstring& mp4_path,
    int width,
    int height) {
    const std::wstring cache = thumbnail_cache_path(mp4_path, width, height);
    if (cache.empty()) {
        return false;
    }
    Gdiplus::Bitmap* bmp = bitmap_from_bgra(pixels);
    if (!bmp) {
        return false;
    }
    CLSID jpeg_clsid{};
    if (get_encoder_clsid(L"image/jpeg", &jpeg_clsid) != 0) {
        delete bmp;
        return false;
    }
    Gdiplus::EncoderParameters params{};
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    ULONG quality = 82;
    params.Parameter[0].Value = &quality;
    const bool ok = bmp->Save(cache.c_str(), &jpeg_clsid, &params) == Gdiplus::Ok;
    delete bmp;
    return ok;
}

}  // namespace ir::gui
