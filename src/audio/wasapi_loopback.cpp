#include "audio/wasapi_loopback.h"

#include "util/log.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

using Microsoft::WRL::ComPtr;

namespace ir::audio {
namespace {

// PKEY_Device_FriendlyName without pulling functiondiscoverykeys_devpkey.h
// (that header fights with DEFINE_PROPERTYKEY / INITGUID under MSVC).
constexpr PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

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

float device_peak(IMMDevice* device) {
    ComPtr<IAudioMeterInformation> meter;
    if (FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &meter))) {
        return 0.0f;
    }
    float peak = 0.0f;
    if (FAILED(meter->GetPeakValue(&peak))) {
        return 0.0f;
    }
    return peak;
}

int count_active_sessions(IMMDevice* device) {
    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr))) {
        return 0;
    }
    ComPtr<IAudioSessionEnumerator> enums;
    if (FAILED(mgr->GetSessionEnumerator(&enums))) {
        return 0;
    }
    int count = 0;
    int n = 0;
    enums->GetCount(&n);
    for (int i = 0; i < n; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(enums->GetSession(i, &ctrl))) {
            continue;
        }
        AudioSessionState state = AudioSessionStateExpired;
        if (SUCCEEDED(ctrl->GetState(&state)) && state == AudioSessionStateActive) {
            ++count;
        }
    }
    return count;
}

struct Candidate {
    ComPtr<IMMDevice> device;
    std::string id;
    std::string name;
    float peak = 0.0f;
    int sessions = 0;
    bool is_default = false;
};

// Score: audible peak wins; then active sessions; default is tie-breaker.
float score_candidate(const Candidate& c) {
    float s = 0.0f;
    if (c.peak > 0.001f) {
        s += 1000.0f + c.peak * 100.0f;
    }
    s += static_cast<float>(c.sessions) * 10.0f;
    if (c.is_default) {
        s += 1.0f;
    }
    return s;
}

}  // namespace

struct WasapiLoopback::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mix_format = nullptr;
    SwrContext* swr = nullptr;
    std::vector<uint8_t> convert_buf;
    bool com_inited = false;
    bool direct_copy = false;
    AVSampleFormat src_fmt = AV_SAMPLE_FMT_NONE;
};

WasapiLoopback::~WasapiLoopback() {
    stop();
}

bool WasapiLoopback::pick_and_open() {
    if (!impl_ || !impl_->enumerator) {
        return false;
    }

    ComPtr<IMMDevice> default_dev;
    std::string default_id;
    if (SUCCEEDED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &default_dev)) ||
        SUCCEEDED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_dev))) {
        default_id = device_id_utf8(default_dev.Get());
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = impl_->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        ir::log::error("EnumAudioEndpoints failed: " + hr_hex(hr));
        return false;
    }

    UINT count = 0;
    collection->GetCount(&count);
    if (count == 0) {
        ir::log::error("No active playback (render) devices");
        return false;
    }

    // Give meters a moment to reflect current mix (Chrome may have just started).
    Sleep(80);

    std::vector<Candidate> cands;
    cands.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev))) {
            continue;
        }
        Candidate c;
        c.device = dev;
        c.id = device_id_utf8(dev.Get());
        c.name = device_friendly_name(dev.Get());
        c.peak = device_peak(dev.Get());
        c.sessions = count_active_sessions(dev.Get());
        c.is_default = (!default_id.empty() && c.id == default_id);
        cands.push_back(std::move(c));
    }

    if (cands.empty()) {
        return false;
    }

    auto best = std::max_element(
        cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
            return score_candidate(a) < score_candidate(b);
        });

    for (const auto& c : cands) {
        ir::log::info(
            std::string("  audio device: \"") + c.name + "\" peak=" + std::to_string(c.peak) +
            " sessions=" + std::to_string(c.sessions) + (c.is_default ? " [default]" : "") +
            (c.id == best->id ? " <- selected" : ""));
    }

    impl_->device = best->device;
    device_id_ = best->id;
    device_name_ = best->name;

    if (best->peak < 0.001f && best->sessions == 0) {
        ir::log::info(
            "No device is currently playing — using \"" + device_name_ +
            "\" (will auto-switch when another device gets audio)");
    } else {
        ir::log::info("Capturing from playback device: \"" + device_name_ + "\"");
    }

    return open_on_device();
}

bool WasapiLoopback::open_on_device() {
    if (!impl_ || !impl_->device) {
        return false;
    }

    // Tear down previous client if reopening.
    if (impl_->client) {
        impl_->client->Stop();
    }
    if (impl_->swr) {
        swr_free(&impl_->swr);
    }
    if (impl_->mix_format) {
        CoTaskMemFree(impl_->mix_format);
        impl_->mix_format = nullptr;
    }
    impl_->capture.Reset();
    impl_->client.Reset();
    impl_->direct_copy = false;

    HRESULT hr = impl_->device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, &impl_->client);
    if (FAILED(hr)) {
        ir::log::error("IAudioClient Activate failed: " + hr_hex(hr));
        return false;
    }

    hr = impl_->client->GetMixFormat(&impl_->mix_format);
    if (FAILED(hr) || !impl_->mix_format) {
        ir::log::error("GetMixFormat failed: " + hr_hex(hr));
        return false;
    }

    constexpr REFERENCE_TIME kBufferDuration = 50 * 10000;  // 50 ms
    hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        kBufferDuration,
        0,
        impl_->mix_format,
        nullptr);
    if (FAILED(hr)) {
        ir::log::error("IAudioClient Initialize(LOOPBACK) failed: " + hr_hex(hr));
        return false;
    }

    hr = impl_->client->GetService(__uuidof(IAudioCaptureClient), &impl_->capture);
    if (FAILED(hr)) {
        ir::log::error("GetService(IAudioCaptureClient) failed: " + hr_hex(hr));
        return false;
    }

    format_.sample_rate = 48000;
    format_.channels = 2;

    AVChannelLayout src_ch{};
    AVChannelLayout dst_ch{};
    av_channel_layout_default(&src_ch, impl_->mix_format->nChannels);
    av_channel_layout_default(&dst_ch, format_.channels);

    AVSampleFormat src_fmt = AV_SAMPLE_FMT_FLT;
    if (impl_->mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        src_fmt = AV_SAMPLE_FMT_FLT;
    } else if (impl_->mix_format->wFormatTag == WAVE_FORMAT_PCM &&
               impl_->mix_format->wBitsPerSample == 16) {
        src_fmt = AV_SAMPLE_FMT_S16;
    } else if (impl_->mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(impl_->mix_format);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            src_fmt = AV_SAMPLE_FMT_FLT;
        } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
                   impl_->mix_format->wBitsPerSample == 16) {
            src_fmt = AV_SAMPLE_FMT_S16;
        } else {
            ir::log::error("Unsupported WAVEFORMATEXTENSIBLE mix format");
            return false;
        }
    } else {
        ir::log::error("Unsupported mix format tag=" + std::to_string(impl_->mix_format->wFormatTag));
        return false;
    }
    impl_->src_fmt = src_fmt;

    impl_->direct_copy =
        src_fmt == AV_SAMPLE_FMT_FLT &&
        impl_->mix_format->nSamplesPerSec == static_cast<DWORD>(format_.sample_rate) &&
        impl_->mix_format->nChannels == static_cast<WORD>(format_.channels);

    if (!impl_->direct_copy) {
        swr_alloc_set_opts2(
            &impl_->swr,
            &dst_ch,
            AV_SAMPLE_FMT_FLT,
            format_.sample_rate,
            &src_ch,
            src_fmt,
            impl_->mix_format->nSamplesPerSec,
            0,
            nullptr);
        if (!impl_->swr || swr_init(impl_->swr) < 0) {
            ir::log::error("swr_init for WASAPI->48k float failed");
            return false;
        }
    }

    hr = impl_->client->Start();
    if (FAILED(hr)) {
        ir::log::error("IAudioClient Start failed: " + hr_hex(hr));
        return false;
    }

    silence_ticks_ = 0;
    ir::log::info(
        "WASAPI loopback ready on \"" + device_name_ + "\": mix " +
        std::to_string(impl_->mix_format->nChannels) + "ch @" +
        std::to_string(impl_->mix_format->nSamplesPerSec) + " Hz -> out " +
        std::to_string(format_.channels) + "ch @" + std::to_string(format_.sample_rate) + " Hz" +
        (impl_->direct_copy ? " (direct)" : " (swresample)"));
    return true;
}

bool WasapiLoopback::start() {
    return start_preferred({}, true);
}

bool WasapiLoopback::start_preferred(const std::string& preferred_id, bool allow_auto_switch) {
    stop();
    impl_ = new Impl();
    allow_auto_switch_ = allow_auto_switch;
    prefer_locked_ = !preferred_id.empty();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        impl_->com_inited = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        ir::log::error("CoInitializeEx failed: " + hr_hex(hr));
        stop();
        return false;
    }

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        &impl_->enumerator);
    if (FAILED(hr)) {
        ir::log::error("MMDeviceEnumerator create failed: " + hr_hex(hr));
        stop();
        return false;
    }

    if (!preferred_id.empty()) {
        // Open by id if possible.
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, preferred_id.c_str(), -1, nullptr, 0);
        if (wlen > 1) {
            std::wstring wid(static_cast<size_t>(wlen - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, preferred_id.c_str(), -1, wid.data(), wlen);
            ComPtr<IMMDevice> preferred;
            if (SUCCEEDED(impl_->enumerator->GetDevice(wid.c_str(), &preferred)) && preferred) {
                impl_->device = preferred;
                device_id_ = preferred_id;
                device_name_ = device_friendly_name(preferred.Get());
                ir::log::info("Using preferred playback device: \"" + device_name_ + "\"");
                if (open_on_device()) {
                    running_ = true;
                    return true;
                }
                ir::log::warn("Preferred audio device failed — falling back to auto");
            } else {
                ir::log::warn("Preferred audio device id not found — falling back to auto");
            }
        }
    }

    ir::log::info("Scanning playback devices for active audio...");
    if (!pick_and_open()) {
        stop();
        return false;
    }

    running_ = true;
    return true;
}

void WasapiLoopback::stop() {
    running_ = false;
    device_name_.clear();
    device_id_.clear();
    silence_ticks_ = 0;
    allow_auto_switch_ = true;
    prefer_locked_ = false;
    if (!impl_) {
        return;
    }
    if (impl_->client) {
        impl_->client->Stop();
    }
    if (impl_->swr) {
        swr_free(&impl_->swr);
    }
    if (impl_->mix_format) {
        CoTaskMemFree(impl_->mix_format);
        impl_->mix_format = nullptr;
    }
    impl_->capture.Reset();
    impl_->client.Reset();
    impl_->device.Reset();
    impl_->enumerator.Reset();
    if (impl_->com_inited) {
        CoUninitialize();
    }
    delete impl_;
    impl_ = nullptr;
}

bool WasapiLoopback::reopen_if_better_device() {
    if (!allow_auto_switch_ || prefer_locked_) {
        return false;
    }
    if (!impl_ || !impl_->enumerator) {
        return false;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(impl_->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return false;
    }

    UINT count = 0;
    collection->GetCount(&count);

    float best_peak = 0.0f;
    ComPtr<IMMDevice> best;
    std::string best_id;
    std::string best_name;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev))) {
            continue;
        }
        const float peak = device_peak(dev.Get());
        if (peak > best_peak) {
            best_peak = peak;
            best = dev;
            best_id = device_id_utf8(dev.Get());
            best_name = device_friendly_name(dev.Get());
        }
    }

    // Only switch if another device is clearly playing and ours is quiet.
    constexpr float kAudible = 0.01f;
    if (!best || best_peak < kAudible) {
        return false;
    }
    if (best_id == device_id_) {
        return false;
    }

    const float current_peak = impl_->device ? device_peak(impl_->device.Get()) : 0.0f;
    if (current_peak >= kAudible * 0.5f) {
        return false;  // current device still has audio
    }

    ir::log::info(
        "Audio moved to \"" + best_name + "\" (peak=" + std::to_string(best_peak) +
        ") — switching loopback");

    impl_->device = best;
    device_id_ = best_id;
    device_name_ = best_name;
    return open_on_device();
}

int WasapiLoopback::read(std::vector<PcmChunk>& out_chunks) {
    if (!running_ || !impl_ || !impl_->capture) {
        return 0;
    }

    int total_frames_out = 0;
    float chunk_peak = 0.0f;

    while (true) {
        UINT32 packet_frames = 0;
        HRESULT hr = impl_->capture->GetNextPacketSize(&packet_frames);
        if (FAILED(hr) || packet_frames == 0) {
            break;
        }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 device_pos = 0;
        UINT64 qpc_pos = 0;
        hr = impl_->capture->GetBuffer(&data, &frames, &flags, &device_pos, &qpc_pos);
        if (FAILED(hr)) {
            ir::log::warn("IAudioCaptureClient::GetBuffer failed: " + hr_hex(hr));
            break;
        }

        PcmChunk chunk;
        chunk.qpc = (qpc_pos != 0) ? static_cast<int64_t>(qpc_pos) : 0;
        if (chunk.qpc == 0) {
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            chunk.qpc = c.QuadPart;
        }

        if (impl_->direct_copy) {
            const size_t samples = static_cast<size_t>(frames) * format_.channels;
            chunk.interleaved.resize(samples);
            float* dst = chunk.interleaved.data();

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                for (size_t i = 0; i < samples; ++i) {
                    dst[i] = 0.0f;
                }
            } else {
                std::memcpy(
                    dst,
                    data,
                    static_cast<size_t>(frames) * impl_->mix_format->nBlockAlign);
                for (size_t i = 0; i < samples; ++i) {
                    chunk_peak = (std::max)(chunk_peak, std::fabs(dst[i]));
                }
            }

            impl_->capture->ReleaseBuffer(frames);
            chunk.frames = static_cast<int>(frames);
            total_frames_out += chunk.frames;
            out_chunks.push_back(std::move(chunk));
            continue;
        }

        if (!impl_->swr) {
            impl_->capture->ReleaseBuffer(frames);
            break;
        }

        const int src_rate = impl_->mix_format->nSamplesPerSec;
        const int dst_rate = format_.sample_rate;
        int dst_nb_samples = frames;
        if (src_rate != dst_rate) {
            dst_nb_samples = static_cast<int>(
                av_rescale_rnd(frames, dst_rate, src_rate, AV_ROUND_UP));
        }
        dst_nb_samples += 32;

        const int dst_bytes = dst_nb_samples * format_.channels * static_cast<int>(sizeof(float));
        if (static_cast<int>(impl_->convert_buf.size()) < dst_bytes) {
            impl_->convert_buf.resize(static_cast<size_t>(dst_bytes));
        }

        uint8_t* out_planes[1] = {impl_->convert_buf.data()};
        const uint8_t* in_planes[1] = {data};

        std::vector<uint8_t> silence;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            const int src_bytes =
                static_cast<int>(frames) * impl_->mix_format->nBlockAlign;
            silence.assign(static_cast<size_t>(src_bytes), 0);
            in_planes[0] = silence.data();
        }

        const int converted = swr_convert(
            impl_->swr,
            out_planes,
            dst_nb_samples,
            in_planes,
            static_cast<int>(frames));

        impl_->capture->ReleaseBuffer(frames);

        if (converted < 0) {
            ir::log::warn("swr_convert failed");
            break;
        }
        if (converted == 0) {
            continue;
        }

        const float* f = reinterpret_cast<const float*>(impl_->convert_buf.data());
        const size_t floats = static_cast<size_t>(converted) * static_cast<size_t>(format_.channels);
        for (size_t i = 0; i < floats; ++i) {
            chunk_peak = (std::max)(chunk_peak, std::fabs(f[i]));
        }
        chunk.interleaved.assign(f, f + floats);
        chunk.frames = converted;
        total_frames_out += converted;
        out_chunks.push_back(std::move(chunk));
    }

    if (total_frames_out == 0 || chunk_peak < 0.0005f) {
        ++silence_ticks_;
    } else {
        silence_ticks_ = 0;
    }
    if (silence_ticks_ >= 200) {
        silence_ticks_ = 0;
        reopen_if_better_device();
    }

    return total_frames_out;
}

}  // namespace ir::audio
