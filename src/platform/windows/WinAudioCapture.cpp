#include "platform/windows/WinAudioCapture.h"

#include <atomic>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include "core/Clock.h"
#include "core/Logger.h"

namespace TGCapture::platform
{

namespace
{
const CLSID CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID IID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID IID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID IID_IAudioCaptureClient = {
    0xC8ADBD64, 0xE71E, 0x48a0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
}  // namespace

// WASAPI 端点枚举：返回当前可用的麦克风(capture=true) / 扬声器(capture=false) 设备
std::vector<AudioDeviceInfo> ListAudioDevices(bool capture)
{
    std::vector<AudioDeviceInfo> devices;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (needUninit) {
            CoUninitialize();
        }
        return devices;
    }

    // 默认设备 id（用于打 isDefault 标记）
    std::wstring defaultId;
    IMMDevice* defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &defaultDevice))
        && defaultDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        defaultDevice->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection))
        && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) {
                continue;
            }
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id) {
                AudioDeviceInfo info;
                info.id = id;
                info.name = id;   // 端点 id 已包含设备标识，调用方按 id 选择
                info.isDefault = (defaultId == info.id);
                devices.push_back(std::move(info));
                CoTaskMemFree(id);
            }
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    if (needUninit) {
        CoUninitialize();
    }
    return devices;
}

WinAudioCapture::WinAudioCapture(AudioCapSourceType type, bool loopback)
    : capture::AudioSourceBase(type), m_loopback(loopback)
{
}

WinAudioCapture::~WinAudioCapture()
{
    Stop();
    ReleaseWASAPI();
}

int WinAudioCapture::Open(int sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    m_channels = channels > 0 ? channels : 2;
    if (!InitWASAPI()) {
        TG_LOGW("WinAudioCapture: WASAPI 初始化失败，将使用静音兜底");
        return -1;
    }
    return 0;
}

bool WinAudioCapture::InitWASAPI()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        return false;
    }

    const EDataFlow flow = m_loopback ? eRender : eCapture;
    if (!m_deviceId.empty()) {
        // 指定端点（ListAudioDevices 取到的 id）
        hr = enumerator->GetDevice(m_deviceId.c_str(), &m_device);
        if (FAILED(hr) || !m_device) {
            TG_LOGW("WinAudioCapture: 指定设备不可用，回落默认设备");
            hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
        }
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
    }
    enumerator->Release();
    if (FAILED(hr) || !m_device) {
        return false;
    }

    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr) || !m_audioClient) {
        return false;
    }

    WAVEFORMATEX* waveFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr) || !waveFormat) {
        return false;
    }

    m_deviceChannels = waveFormat->nChannels;
    m_deviceSampleRate = static_cast<int>(waveFormat->nSamplesPerSec);
    m_deviceFloat = waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     reinterpret_cast<WAVEFORMATEXTENSIBLE*>(waveFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    const REFERENCE_TIME duration = 2000000;  // 200ms
    DWORD flags = m_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, waveFormat, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(waveFormat);
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrames);
    hr = m_audioClient->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(&m_captureClient));
    CoTaskMemFree(waveFormat);
    if (FAILED(hr) || !m_captureClient) {
        return false;
    }

    // 重采样上下文：设备格式 -> 目标 S16 交错
    // WASAPI 共享模式下缓冲是交错(interleaved)布局：float -> FLT，PCM16 -> S16
    const AVSampleFormat srcFmt = m_deviceFloat ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
    // ffmpeg 6.x：用 swr_alloc_set_opts2 + AVChannelLayout（swr_alloc_set_opts / av_get_default_channel_layout 已废弃，7.x 移除）
    AVChannelLayout outLayout{};
    AVChannelLayout inLayout{};
    av_channel_layout_default(&outLayout, m_channels > 0 ? m_channels : 2);
    av_channel_layout_default(&inLayout, m_deviceChannels > 0 ? m_deviceChannels : 2);
    const int swrRet = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_S16, m_sampleRate,
                                           &inLayout, srcFmt, m_deviceSampleRate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    av_channel_layout_uninit(&inLayout);
    if (swrRet < 0 || !m_swr || swr_init(m_swr) < 0) {
        TG_LOGW("WinAudioCapture: swr 初始化失败");
        if (m_swr) {
            swr_free(&m_swr);
        }
        return false;
    }

    m_audioClient->Start();
    TG_LOGI("WinAudioCapture: 就绪 loopback=%d %dHz %dch", m_loopback ? 1 : 0, m_deviceSampleRate, m_deviceChannels);
    return true;
}

void WinAudioCapture::ReleaseWASAPI()
{
    if (m_audioClient) {
        m_audioClient->Stop();
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    if (m_swr) {
        swr_free(&m_swr);
    }
    if (m_comInited) {
        CoUninitialize();
        m_comInited = false;
    }
}

void WinAudioCapture::Loop(std::stop_token token)
{
    if (!m_captureClient) {
        // 兜底：产生静音，保证下游时钟推进
        const int frames = m_sampleRate / 100;  // 10ms
        while (!token.stop_requested()) {
            auto frame = std::make_shared<core::Frame>();
            frame->kind = core::MediaKind::Audio;
            frame->sampleRate = m_sampleRate;
            frame->channels = m_channels;
            frame->sampleFmt = AV_SAMPLE_FMT_S16;
            frame->nbSamples = frames;
            frame->audioBytes = static_cast<size_t>(frames) * m_channels * sizeof(int16_t);
            frame->audioData = std::shared_ptr<uint8_t>(new uint8_t[frame->audioBytes](), std::default_delete<uint8_t[]>());
            frame->ptsUs = core::MonotonicClock::NowUs();
            Emit(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }

    while (!token.stop_requested()) {
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            break;
        }
        if (packetLength == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        BYTE* data = nullptr;
        UINT32 framesAvailable = 0;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        hr = m_captureClient->GetBuffer(&data, &framesAvailable, &flags, &devicePosition, &qpcPosition);
        if (FAILED(hr)) {
            break;
        }

        if (framesAvailable > 0 && data) {
            if (m_muted.load()) {
                memset(data, 0, static_cast<size_t>(framesAvailable) * m_deviceChannels *
                                     (m_deviceFloat ? sizeof(float) : sizeof(int16_t)));
            }
            auto frame = std::make_shared<core::Frame>();
            Resample(data, static_cast<int>(framesAvailable), frame);
            if (frame->audioData) {
                frame->ptsUs = core::MonotonicClock::NowUs();
                Emit(std::move(frame));
            }
        }
        m_captureClient->ReleaseBuffer(framesAvailable);
    }
}

void WinAudioCapture::OnThreadExit()
{
    TG_LOGI("WinAudioCapture: 线程退出");
}

void WinAudioCapture::Resample(const uint8_t* src, int frames, core::FramePtr& out)
{
    out->kind = core::MediaKind::Audio;
    out->sampleRate = m_sampleRate;
    out->channels = m_channels;
    out->sampleFmt = AV_SAMPLE_FMT_S16;
    out->planar = false;

    if (!m_swr) {
        return;
    }

    const int dstSamples = swr_get_out_samples(m_swr, frames);
    if (dstSamples <= 0) {
        return;
    }
    const size_t bytes = static_cast<size_t>(dstSamples) * m_channels * sizeof(int16_t);
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());

    const uint8_t* inputs[1] = {src};
    uint8_t* outputs[1] = {buffer.get()};
    int converted = swr_convert(m_swr, outputs, dstSamples, inputs, frames);
    if (converted <= 0) {
        return;
    }

    out->nbSamples = converted;
    out->audioBytes = static_cast<size_t>(converted) * m_channels * sizeof(int16_t);
    out->audioData = std::move(buffer);
}

void WinAudioCapture::SetVolume(float gain)
{
    // 采集侧不做音量（避免设备独占），音量在混音阶段生效
    TG_LOGD("WinAudioCapture: SetVolume %.2f (由混音器处理)", gain);
}

}  // namespace TGCapture::platform
