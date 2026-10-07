#include "capture/gpu_nv12_converter.h"

#include "util/log.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ir::capture {

struct GpuNv12Converter::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> nv12_tex;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    ComPtr<ID3D11Texture2D> staging_nv12;
};

GpuNv12Converter::~GpuNv12Converter() {
    shutdown();
}

bool GpuNv12Converter::init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) {
    shutdown();
    if (!device || !context || width <= 0 || height <= 0) {
        return false;
    }
    // NV12 needs even dimensions.
    if ((width & 1) || (height & 1)) {
        ir::log::error("GpuNv12Converter: size must be even");
        return false;
    }

    impl_ = new Impl();
    impl_->device = device;
    impl_->context = context;
    width_ = width;
    height_ = height;

    HRESULT hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), &impl_->video_device);
    if (FAILED(hr)) {
        ir::log::warn("ID3D11VideoDevice not available — GPU NV12 convert disabled");
        shutdown();
        return false;
    }
    hr = context->QueryInterface(__uuidof(ID3D11VideoContext), &impl_->video_context);
    if (FAILED(hr)) {
        ir::log::warn("ID3D11VideoContext not available");
        shutdown();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = static_cast<UINT>(width);
    content.InputHeight = static_cast<UINT>(height);
    content.OutputWidth = static_cast<UINT>(width);
    content.OutputHeight = static_cast<UINT>(height);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = impl_->video_device->CreateVideoProcessorEnumerator(&content, &impl_->enumerator);
    if (FAILED(hr)) {
        ir::log::warn("CreateVideoProcessorEnumerator failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    impl_->enumerator->GetVideoProcessorCaps(&caps);

    UINT flags = 0;
    hr = impl_->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags);
    if (FAILED(hr) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        ir::log::warn("VideoProcessor does not accept BGRA input");
        shutdown();
        return false;
    }
    hr = impl_->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags);
    if (FAILED(hr) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        ir::log::warn("VideoProcessor does not output NV12");
        shutdown();
        return false;
    }

    hr = impl_->video_device->CreateVideoProcessor(impl_->enumerator.Get(), 0, &impl_->processor);
    if (FAILED(hr)) {
        ir::log::warn("CreateVideoProcessor failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC nv12_desc{};
    nv12_desc.Width = static_cast<UINT>(width);
    nv12_desc.Height = static_cast<UINT>(height);
    nv12_desc.MipLevels = 1;
    nv12_desc.ArraySize = 1;
    nv12_desc.Format = DXGI_FORMAT_NV12;
    nv12_desc.SampleDesc.Count = 1;
    nv12_desc.Usage = D3D11_USAGE_DEFAULT;
    nv12_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    hr = device->CreateTexture2D(&nv12_desc, nullptr, &impl_->nv12_tex);
    if (FAILED(hr)) {
        // Some drivers want UAV/shader resource instead.
        nv12_desc.BindFlags = 0;
        hr = device->CreateTexture2D(&nv12_desc, nullptr, &impl_->nv12_tex);
    }
    if (FAILED(hr)) {
        ir::log::warn("CreateTexture2D(NV12) failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    hr = impl_->video_device->CreateVideoProcessorOutputView(
        impl_->nv12_tex.Get(), impl_->enumerator.Get(), &ovd, &impl_->output_view);
    if (FAILED(hr)) {
        ir::log::warn("CreateVideoProcessorOutputView failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC staging = nv12_desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&staging, nullptr, &impl_->staging_nv12);
    if (FAILED(hr)) {
        ir::log::warn("CreateTexture2D(NV12 staging) failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    ready_ = true;
    ir::log::info(
        "GPU NV12 converter ready (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    return true;
}

void GpuNv12Converter::shutdown() {
    ready_ = false;
    width_ = 0;
    height_ = 0;
    if (impl_) {
        delete impl_;
        impl_ = nullptr;
    }
}

bool GpuNv12Converter::convert(ID3D11Texture2D* src_bgra, ID3D11Texture2D** out_nv12_texture) {
    if (!ready_ || !impl_ || !src_bgra || !out_nv12_texture) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = impl_->video_device->CreateVideoProcessorInputView(
        src_bgra, impl_->enumerator.Get(), &ivd, &input_view);
    if (FAILED(hr)) {
        // Desktop texture may be DXGI_FORMAT_B8G8R8A8_UNORM_SRGB etc. Try copy to BGRA DEFAULT first.
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();

    RECT rect{0, 0, width_, height_};
    impl_->video_context->VideoProcessorSetStreamFrameFormat(
        impl_->processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    impl_->video_context->VideoProcessorSetStreamSourceRect(impl_->processor.Get(), 0, TRUE, &rect);
    impl_->video_context->VideoProcessorSetStreamDestRect(impl_->processor.Get(), 0, TRUE, &rect);
    impl_->video_context->VideoProcessorSetOutputTargetRect(impl_->processor.Get(), TRUE, &rect);

    hr = impl_->video_context->VideoProcessorBlt(
        impl_->processor.Get(), impl_->output_view.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        ir::log::warn("VideoProcessorBlt failed, hr=" + std::to_string(hr));
        return false;
    }

    *out_nv12_texture = impl_->nv12_tex.Get();
    return true;
}

bool GpuNv12Converter::map_nv12_to_cpu(
    uint8_t* y_plane,
    int y_stride,
    uint8_t* uv_plane,
    int uv_stride) {
    if (!ready_ || !impl_ || !y_plane || !uv_plane) {
        return false;
    }

    impl_->context->CopyResource(impl_->staging_nv12.Get(), impl_->nv12_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = impl_->context->Map(impl_->staging_nv12.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ir::log::warn("Map(NV12 staging) failed, hr=" + std::to_string(hr));
        return false;
    }

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const int pitch = static_cast<int>(mapped.RowPitch);

    for (int y = 0; y < height_; ++y) {
        std::memcpy(
            y_plane + static_cast<size_t>(y) * y_stride,
            src + static_cast<size_t>(y) * pitch,
            static_cast<size_t>(width_));
    }

    const uint8_t* src_uv = src + static_cast<size_t>(pitch) * height_;
    for (int y = 0; y < height_ / 2; ++y) {
        std::memcpy(
            uv_plane + static_cast<size_t>(y) * uv_stride,
            src_uv + static_cast<size_t>(y) * pitch,
            static_cast<size_t>(width_));
    }

    impl_->context->Unmap(impl_->staging_nv12.Get(), 0);
    return true;
}

}  // namespace ir::capture
