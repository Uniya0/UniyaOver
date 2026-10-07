#include "capture/desktop_duplicator.h"

#include "util/log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace ir::capture {
namespace {

std::string narrow_from_wide(const wchar_t* wide) {
    if (!wide) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool is_primary_output(IDXGIOutput* output) {
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) {
        return false;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(desc.Monitor, &mi)) {
        return desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
    }
    return (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

}  // namespace

struct DesktopDuplicator::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output1;
    ComPtr<ID3D11Texture2D> gpu_bgra;
    ComPtr<ID3D11Texture2D> staging;
    int tex_w = 0;
    int tex_h = 0;
    DXGI_FORMAT tex_format = DXGI_FORMAT_UNKNOWN;
};

DesktopDuplicator::~DesktopDuplicator() {
    shutdown();
}

ID3D11Device* DesktopDuplicator::device() const {
    return impl_ ? impl_->device.Get() : nullptr;
}

ID3D11DeviceContext* DesktopDuplicator::context() const {
    return impl_ ? impl_->context.Get() : nullptr;
}

bool DesktopDuplicator::init(int monitor_index) {
    shutdown();
    impl_ = new Impl();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory);
    if (FAILED(hr)) {
        ir::log::error("CreateDXGIFactory1 failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    ComPtr<IDXGIAdapter1> chosen_adapter;
    ComPtr<IDXGIOutput> chosen_output;
    ComPtr<IDXGIAdapter1> primary_adapter;
    ComPtr<IDXGIOutput> primary_output;
    int attached_index = 0;

    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_index, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }

            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (!desc.AttachedToDesktop) {
                continue;
            }

            if (is_primary_output(output.Get())) {
                primary_adapter = adapter;
                primary_output = output;
            }
            if (monitor_index >= 0 && attached_index == monitor_index) {
                chosen_adapter = adapter;
                chosen_output = output;
            }
            ++attached_index;
        }
    }

    if (!chosen_adapter || !chosen_output) {
        chosen_adapter = primary_adapter;
        chosen_output = primary_output;
    }

    if (!chosen_adapter || !chosen_output) {
        ir::log::error("No desktop output found for duplication");
        shutdown();
        return false;
    }

    DXGI_ADAPTER_DESC1 adapter_desc{};
    chosen_adapter->GetDesc1(&adapter_desc);
    adapter_name_ = narrow_from_wide(adapter_desc.Description);

    DXGI_OUTPUT_DESC output_desc{};
    chosen_output->GetDesc(&output_desc);
    width_ = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
    height_ = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
    desktop_rect_ = output_desc.DesktopCoordinates;

    D3D_FEATURE_LEVEL feature_level{};
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    hr = D3D11CreateDevice(
        chosen_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kLevels,
        ARRAYSIZE(kLevels),
        D3D11_SDK_VERSION,
        &impl_->device,
        &feature_level,
        &impl_->context);

    if (FAILED(hr)) {
        ir::log::error("D3D11CreateDevice failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    hr = chosen_output.As(&impl_->output1);
    if (FAILED(hr)) {
        ir::log::error("IDXGIOutput1 query failed, hr=" + std::to_string(hr));
        shutdown();
        return false;
    }

    impl_->adapter = chosen_adapter;

    if (!create_duplication()) {
        shutdown();
        return false;
    }

    ir::log::info(
        "Desktop Duplication ready: " + std::to_string(width_) + "x" + std::to_string(height_) +
        " on \"" + adapter_name_ + "\" (monitor_index=" +
        std::to_string(monitor_index) + ")");
    return true;
}

void DesktopDuplicator::shutdown() {
    release_duplication();
    if (impl_) {
        delete impl_;
        impl_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    adapter_name_.clear();
}

bool DesktopDuplicator::create_duplication() {
    release_duplication();
    if (!impl_ || !impl_->device || !impl_->output1) {
        return false;
    }
    HRESULT hr = impl_->output1->DuplicateOutput(impl_->device.Get(), &impl_->duplication);
    if (FAILED(hr)) {
        ir::log::error("DuplicateOutput failed, hr=" + std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    return true;
}

void DesktopDuplicator::release_duplication() {
    if (impl_) {
        impl_->duplication.Reset();
    }
}

bool DesktopDuplicator::ensure_gpu_bgra(int width, int height, unsigned dxgi_format) {
    const auto format = static_cast<DXGI_FORMAT>(dxgi_format);
    if (impl_->gpu_bgra && impl_->tex_w == width && impl_->tex_h == height &&
        impl_->tex_format == format) {
        return true;
    }

    impl_->gpu_bgra.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = impl_->device->CreateTexture2D(&desc, nullptr, &impl_->gpu_bgra);
    if (FAILED(hr)) {
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = impl_->device->CreateTexture2D(&desc, nullptr, &impl_->gpu_bgra);
    }
    if (FAILED(hr)) {
        ir::log::error("CreateTexture2D(gpu BGRA) failed, hr=" + std::to_string(hr));
        return false;
    }

    impl_->tex_w = width;
    impl_->tex_h = height;
    impl_->tex_format = format;
    return true;
}

bool DesktopDuplicator::ensure_staging(int width, int height, unsigned dxgi_format) {
    const auto format = static_cast<DXGI_FORMAT>(dxgi_format);
    if (impl_->staging) {
        D3D11_TEXTURE2D_DESC d{};
        impl_->staging->GetDesc(&d);
        if (static_cast<int>(d.Width) == width && static_cast<int>(d.Height) == height &&
            d.Format == format) {
            return true;
        }
    }

    impl_->staging.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = impl_->device->CreateTexture2D(&desc, nullptr, &impl_->staging);
    if (FAILED(hr)) {
        ir::log::error("CreateTexture2D(staging) failed, hr=" + std::to_string(hr));
        return false;
    }
    return true;
}

bool DesktopDuplicator::copy_texture_to_cpu(ID3D11Texture2D* src, FrameBuffer& out_frame) {
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    if (!ensure_staging(static_cast<int>(desc.Width), static_cast<int>(desc.Height), desc.Format)) {
        return false;
    }

    impl_->context->CopyResource(impl_->staging.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = impl_->context->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    const int width = static_cast<int>(desc.Width);
    const int height = static_cast<int>(desc.Height);
    const int src_pitch = static_cast<int>(mapped.RowPitch);
    const int dst_stride = width * 4;

    out_frame.width = width;
    out_frame.height = height;
    out_frame.stride_bytes = dst_stride;
    out_frame.bgra.resize(static_cast<size_t>(dst_stride) * static_cast<size_t>(height));

    const auto* src_bytes = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst_bytes = out_frame.bgra.data();
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            dst_bytes + static_cast<size_t>(y) * dst_stride,
            src_bytes + static_cast<size_t>(y) * src_pitch,
            static_cast<size_t>(dst_stride));
    }

    impl_->context->Unmap(impl_->staging.Get(), 0);
    return true;
}

bool DesktopDuplicator::acquire_frame(
    CaptureStats& stats,
    ID3D11Texture2D** out_gpu_bgra,
    FrameBuffer* out_cpu,
    unsigned timeout_ms) {
    if (!impl_ || !impl_->duplication || !out_gpu_bgra) {
        stats.errors++;
        return false;
    }

    stats.width = width_;
    stats.height = height_;
    *out_gpu_bgra = nullptr;

    ComPtr<IDXGIResource> desktop_resource;
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    HRESULT hr = impl_->duplication->AcquireNextFrame(timeout_ms, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        stats.timeouts++;
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        stats.access_lost++;
        ir::log::warn("Desktop duplication access lost; recreating...");
        if (!create_duplication()) {
            stats.errors++;
        }
        return false;
    }
    if (FAILED(hr)) {
        stats.errors++;
        ir::log::error("AcquireNextFrame failed, hr=" + std::to_string(hr));
        return false;
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = desktop_resource.As(&texture);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (ensure_gpu_bgra(static_cast<int>(desc.Width), static_cast<int>(desc.Height), desc.Format)) {
            impl_->context->CopyResource(impl_->gpu_bgra.Get(), texture.Get());
            *out_gpu_bgra = impl_->gpu_bgra.Get();
            ok = true;
            if (out_cpu) {
                // Only used as fallback; skip if GPU path works.
            }
        }
    } else {
        stats.errors++;
    }

    desktop_resource.Reset();
    texture.Reset();
    hr = impl_->duplication->ReleaseFrame();
    if (FAILED(hr)) {
        stats.errors++;
        create_duplication();
        return false;
    }

    if (!ok) {
        stats.errors++;
        return false;
    }

    stats.frames_acquired++;
    return true;
}

}  // namespace ir::capture
