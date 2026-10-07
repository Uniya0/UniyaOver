#include "util/devices.h"

#include "gui/gdi_utils.h"
#include "util/log.h"

#include <dxgi.h>
#include <wrl/client.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace ir::devices {
namespace {

constexpr PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

std::string narrow_from_bstr(LPWSTR wide) {
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

std::string device_friendly_name(IMMDevice* device) {
    if (!device) {
        return "unknown";
    }
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return "unknown";
    }
    PROPVARIANT var{};
    PropVariantInit(&var);
    std::string name = "unknown";
    if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &var)) && var.vt == VT_LPWSTR) {
        name = narrow_from_bstr(var.pwszVal);
    }
    PropVariantClear(&var);
    return name;
}

std::string device_id_utf8(IMMDevice* device) {
    if (!device) {
        return {};
    }
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) {
        return {};
    }
    std::string out = narrow_from_bstr(id);
    CoTaskMemFree(id);
    return out;
}

bool is_primary_output(IDXGIOutput* output) {
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) {
        return false;
    }
    return desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
}

}  // namespace

std::vector<MonitorInfo> list_monitors() {
    std::vector<MonitorInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory))) {
        return out;
    }

    int index = 0;
    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> output;
            const HRESULT hr = adapter->EnumOutputs(oi, &output);
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
            MonitorInfo m;
            m.index = index++;
            m.is_primary = is_primary_output(output.Get());
            m.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            m.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            m.name = narrow_from_bstr(desc.DeviceName);
            wchar_t buf[128];
            std::swprintf(
                buf,
                128,
                L"%d: %dx%d%s",
                m.index,
                m.width,
                m.height,
                m.is_primary ? L" (основной)" : L"");
            m.label = buf;
            out.push_back(std::move(m));
        }
    }
    return out;
}

std::vector<AudioDeviceInfo> list_playback_devices() {
    std::vector<AudioDeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            &enumerator))) {
        return out;
    }

    std::string default_id;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &def)) ||
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        default_id = device_id_utf8(def.Get());
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev))) {
            continue;
        }
        AudioDeviceInfo a;
        a.id = device_id_utf8(dev.Get());
        a.name = device_friendly_name(dev.Get());
        a.is_default = (!default_id.empty() && a.id == default_id);
        a.label = ir::gui::utf8_to_wide(a.name);
        if (a.is_default) {
            a.label += L" [default]";
        }
        out.push_back(std::move(a));
    }
    return out;
}

}  // namespace ir::devices
