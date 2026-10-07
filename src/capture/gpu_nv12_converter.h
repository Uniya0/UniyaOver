#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ir::capture {

// Converts a BGRA GPU texture to NV12 on the GPU (D3D11 Video Processor).
// Output can be mapped to CPU as NV12 planes for NVENC without sws_scale.
class GpuNv12Converter {
public:
    GpuNv12Converter() = default;
    ~GpuNv12Converter();

    GpuNv12Converter(const GpuNv12Converter&) = delete;
    GpuNv12Converter& operator=(const GpuNv12Converter&) = delete;

    bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);
    void shutdown();

    // src must be B8G8R8A8_UNORM (or compatible desktop format).
    // On success, out_nv12_texture is a GPU NV12 texture (caller must not free).
    bool convert(ID3D11Texture2D* src_bgra, ID3D11Texture2D** out_nv12_texture);

    // Map the last converted NV12 frame into tightly packed CPU buffers.
    // y plane size = width*height, uv plane size = width*height/2.
    bool map_nv12_to_cpu(uint8_t* y_plane, int y_stride, uint8_t* uv_plane, int uv_stride);

    bool is_ready() const { return ready_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace ir::capture
