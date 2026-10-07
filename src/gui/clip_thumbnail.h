#pragma once

#include "gui/gdi_utils.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ir::gui {

struct ThumbnailPixels {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4
};

// %LOCALAPPDATA%/UniyaOver/thumbs/
std::wstring thumbs_directory();

// Path of the JPEG cache entry for this clip (empty if the source file is missing).
std::wstring thumbnail_cache_path(
    const std::wstring& mp4_path,
    int width = 172,
    int height = 96);

// True if a valid JPEG already exists for the current size/mtime of the clip.
bool thumbnail_cache_valid(
    const std::wstring& mp4_path,
    int width = 172,
    int height = 96);

// FFmpeg-only decode (safe on a worker thread). Fills BGRA cover-cropped to WxH.
bool decode_clip_thumbnail_bgra(
    const std::wstring& mp4_path,
    int width,
    int height,
    ThumbnailPixels& out);

// UI-thread helpers (GDI+).
Gdiplus::Bitmap* bitmap_from_bgra(const ThumbnailPixels& pixels);
Gdiplus::Bitmap* load_thumbnail_bitmap_from_cache(
    const std::wstring& mp4_path,
    int width = 172,
    int height = 96);
bool save_thumbnail_jpeg(
    const ThumbnailPixels& pixels,
    const std::wstring& mp4_path,
    int width = 172,
    int height = 96);

}  // namespace ir::gui
