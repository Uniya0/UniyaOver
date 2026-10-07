#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ir::capture {

struct CaptureStats {
    uint64_t frames_acquired = 0;
    uint64_t timeouts = 0;
    uint64_t access_lost = 0;
    uint64_t errors = 0;
    uint64_t frames_encoded = 0;
    int width = 0;
    int height = 0;
    double updates_per_second = 0.0;
};

// CPU-side BGRA (fallback path).
struct FrameBuffer {
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    std::vector<uint8_t> bgra;

    bool empty() const { return bgra.empty() || width <= 0 || height <= 0; }
};

class DesktopDuplicator {
public:
    DesktopDuplicator() = default;
    ~DesktopDuplicator();

    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;

    // monitor_index: -1 = primary; otherwise 0-based across attached outputs.
    bool init(int monitor_index = -1);
    void shutdown();

    // Copies the newest desktop image into an owned GPU BGRA texture.
    // Returns that texture via out_gpu_bgra (do not Release; owned by duplicator).
    // Also fills out_cpu if non-null (optional fallback).
    bool acquire_frame(
        CaptureStats& stats,
        ID3D11Texture2D** out_gpu_bgra,
        FrameBuffer* out_cpu,
        unsigned timeout_ms);

    ID3D11Device* device() const;
    ID3D11DeviceContext* context() const;

    // CPU BGRA fallback when GPU NV12 convert fails.
    bool copy_texture_to_cpu(ID3D11Texture2D* src, FrameBuffer& out_frame);

    bool is_initialized() const { return impl_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    RECT desktop_rect() const { return desktop_rect_; }
    std::string adapter_name() const { return adapter_name_; }

private:
    bool create_duplication();
    void release_duplication();
    bool ensure_gpu_bgra(int width, int height, unsigned dxgi_format);
    bool ensure_staging(int width, int height, unsigned dxgi_format);

    struct Impl;
    Impl* impl_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    RECT desktop_rect_{};
    std::string adapter_name_;
};

}  // namespace ir::capture
