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

// ------------------------------------------------------------------
// 轻量级 IMMNotificationClient：只关心默认设备切换
// 回调仅置原子标志，RetryOpen 由工作线程在安全时机执行（对齐 MediaX P0 修复）
// ------------------------------------------------------------------
class DeviceNotifyClient final : public IMMNotificationClient
{
public:
    DeviceNotifyClient(std::atomic<bool>* flag, bool loopback) noexcept
        : m_flag(flag), m_loopback(loopback) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return m_ref.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = m_ref.fetch_sub(1) - 1;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient：默认设备切换时置延迟标志
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (role != eMultimedia && role != eConsole) return S_OK;
        // 麦克风：flow==eCapture；loopback 扬声器：flow==eRender
        const bool match = m_loopback ? (flow == eRender) : (flow == eCapture);
        if (match && m_flag) {
            TG_LOGI("WinAudioCapture: 默认设备变更 flow=%d role=%d → 延迟 RetryOpen", flow, role);
            m_flag->store(true);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> m_ref{1};
    std::atomic<bool>* m_flag;
    bool m_loopback;
};

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
    // 只有"本次调用真的完成了初始化"才记账，将来才配一次 CoUninitialize。
    // RPC_E_CHANGED_MODE 表示本线程已以另一种模式初始化过 COM —— 它**失败且不增加引用计数**，
    // 当成功记账会让 ReleaseWASAPI 多调一次 CoUninitialize，把宿主线程的 apartment 计数打负，
    // 表现为宿主其它 COM 调用随机失败。多次 Open 同理：只认第一次。
    if (!m_comInited) {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInited = SUCCEEDED(comHr);
        if (comHr == RPC_E_CHANGED_MODE) {
            TG_LOGW("WinAudioCapture: COM 已由宿主以其它模式初始化，本对象不接管其生命周期");
        }
    }
    HRESULT hr = S_OK;

    // 枚举器保留为成员：既用于首次打开，也用于后续默认设备变更的 RetryOpen 以及反注册回调。
    if (!m_enumerator) {
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                              reinterpret_cast<void**>(&m_enumerator));
        if (FAILED(hr) || !m_enumerator) {
            return false;
        }
    }

    // 下面每个失败分支都走 ReleaseEndpoint()：原先是直接 return，
    // 于是"设备拿到了但 Activate 失败""client 建好了但 Initialize 失败"这些中途态
    // 会把 COM 对象一路留着，重试时再被新指针覆盖 —— 每失败一轮泄漏一个 IMMDevice
    // 加一个已 Initialize 的 IAudioClient（后者还占着音频端点不放）。
    const EDataFlow flow = m_loopback ? eRender : eCapture;
    if (m_device) {
        // 重入保护：失败重试路径可能带着上一轮的 device 进来，覆写前先放掉
        m_device->Release();
        m_device = nullptr;
    }
    if (!m_deviceId.empty()) {
        // 指定端点（ListAudioDevices 取到的 id）
        hr = m_enumerator->GetDevice(m_deviceId.c_str(), &m_device);
        if (FAILED(hr) || !m_device) {
            TG_LOGW("WinAudioCapture: 指定设备不可用，回落默认设备");
            if (m_device) {
                m_device->Release();
                m_device = nullptr;
            }
            hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
        }
    } else {
        hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
    }
    if (FAILED(hr) || !m_device) {
        ReleaseEndpoint();
        return false;
    }

    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr) || !m_audioClient) {
        ReleaseEndpoint();
        return false;
    }

    WAVEFORMATEX* waveFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr) || !waveFormat) {
        ReleaseEndpoint();
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
        ReleaseEndpoint();
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrames);
    hr = m_audioClient->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(&m_captureClient));
    CoTaskMemFree(waveFormat);
    if (FAILED(hr) || !m_captureClient) {
        ReleaseEndpoint();   // 此时 audioClient 已 Initialize，不放就一直占着端点
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
        ReleaseEndpoint();   // 原先只放了 swr，device / audioClient / captureClient 全留着
        return false;
    }

    m_audioClient->Start();
    TG_LOGI("WinAudioCapture: 就绪 loopback=%d %dHz %dch", m_loopback ? 1 : 0, m_deviceSampleRate, m_deviceChannels);

    // 首次打开成功后注册 IMMNotificationClient（已注册则跳过）
    RegisterDeviceNotify();
    return true;
}

void WinAudioCapture::RegisterDeviceNotify()
{
    if (!m_enumerator || m_notifyClient) return;
    auto* client = new DeviceNotifyClient(&m_needRetryOpen, m_loopback);
    HRESULT hr = m_enumerator->RegisterEndpointNotificationCallback(client);
    if (FAILED(hr)) {
        TG_LOGW("WinAudioCapture: RegisterEndpointNotificationCallback 失败 hr=0x%08X",
                static_cast<unsigned>(hr));
        client->Release();
        return;
    }
    m_notifyClient = client;
}

void WinAudioCapture::UnregisterDeviceNotify()
{
    if (!m_enumerator || !m_notifyClient) return;
    auto* client = static_cast<DeviceNotifyClient*>(m_notifyClient);
    m_enumerator->UnregisterEndpointNotificationCallback(client);
    client->Release();
    m_notifyClient = nullptr;
}

void WinAudioCapture::ReleaseEndpoint()
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
        m_swr = nullptr;
    }
    m_bufferFrames = 0;
}

void WinAudioCapture::Close()
{
    ReleaseWASAPI();
}

void WinAudioCapture::ReleaseWASAPI()
{
    // 回调要先摘，不然 COM 线程可能在采集对象释放后还回来置标志
    UnregisterDeviceNotify();
    ReleaseEndpoint();
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
    if (m_comInited) {
        CoUninitialize();
        m_comInited = false;
    }
}

// 工作线程安全时机重开 AudioClient：对齐 MediaX::WinMMCapturer::RetryOpen。
// 释放 AudioClient / CaptureClient / Device（保留 enumerator、COM 已 Init、notifyClient），
// 重新 GetDefaultAudioEndpoint + Activate + Initialize + Start。
int WinAudioCapture::RetryOpen()
{
    TG_LOGI("WinAudioCapture: RetryOpen 开始 loopback=%d", m_loopback ? 1 : 0);
    ReleaseEndpoint();

    const EDataFlow flow = m_loopback ? eRender : eCapture;
    HRESULT hr = m_enumerator ? m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device) : E_FAIL;
    if (FAILED(hr) || !m_device) {
        TG_LOGW("WinAudioCapture: RetryOpen GetDefaultAudioEndpoint 失败 hr=0x%08X",
                static_cast<unsigned>(hr));
        ReleaseEndpoint();
        return -1;
    }
    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr) || !m_audioClient) {
        TG_LOGW("WinAudioCapture: RetryOpen Activate 失败 hr=0x%08X", static_cast<unsigned>(hr));
        ReleaseEndpoint();
        return -1;
    }

    WAVEFORMATEX* waveFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr) || !waveFormat) {
        TG_LOGW("WinAudioCapture: RetryOpen GetMixFormat 失败");
        ReleaseEndpoint();
        return -1;
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
        TG_LOGW("WinAudioCapture: RetryOpen Initialize 失败 hr=0x%08X", static_cast<unsigned>(hr));
        ReleaseEndpoint();
        return -1;
    }
    m_audioClient->GetBufferSize(&m_bufferFrames);
    hr = m_audioClient->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(&m_captureClient));
    const AVSampleFormat srcFmt = m_deviceFloat ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
    AVChannelLayout outLayout{};
    AVChannelLayout inLayout{};
    av_channel_layout_default(&outLayout, m_channels > 0 ? m_channels : 2);
    av_channel_layout_default(&inLayout, m_deviceChannels > 0 ? m_deviceChannels : 2);
    const int swrRet = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_S16, m_sampleRate,
                                           &inLayout, srcFmt, m_deviceSampleRate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    av_channel_layout_uninit(&inLayout);
    CoTaskMemFree(waveFormat);
    if (FAILED(hr) || !m_captureClient || swrRet < 0 || !m_swr || swr_init(m_swr) < 0) {
        TG_LOGW("WinAudioCapture: RetryOpen 后端重建失败");
        ReleaseEndpoint();   // 不只放 swr：失败时 client / device 也必须还回去
        return -1;
    }
    m_audioClient->Start();
    TG_LOGI("WinAudioCapture: RetryOpen 完成 %dHz %dch", m_deviceSampleRate, m_deviceChannels);
    return 0;
}

void WinAudioCapture::Loop(std::stop_token token)
{
    if (!m_captureClient) {
        SilenceLoop(token);
        return;
    }

    // loopback 心跳：loopback 没数据时每 ~10ms 发一帧静音，保持音频时间轴连续
    // （麦克风路径一般会稳定出数据，但偶发断流也走同一机制，不会多发 10 条心跳）
    const int silenceFrames = m_sampleRate > 0 ? m_sampleRate / 100 : 441;
    const int64_t silenceIntervalUs = 10 * 1000;
    m_lastEmitUs = core::MonotonicClock::NowUs();

    while (!token.stop_requested()) {
        // P0 修复：在工作线程安全时机处理默认设备变更（延迟 RetryOpen）
        if (m_needRetryOpen.exchange(false)) {
            if (RetryOpen() != 0) {
                TG_LOGW("WinAudioCapture: 延迟 RetryOpen 失败，下轮重试");
                m_needRetryOpen.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // 不论成功失败都继续循环（失败路径仍会推心跳保证连续）
            continue;
        }

        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient ? m_captureClient->GetNextPacketSize(&packetLength) : E_FAIL;
        if (FAILED(hr)) {
            // 设备被拔掉/独占/失效：不能让音频时间轴断掉，转静音兜底
            TG_LOGW("WinAudioCapture: 采集失败 hr=0x%08X，转静音兜底", static_cast<unsigned>(hr));
            break;
        }
        if (packetLength == 0) {
            // 静默心跳：对齐 MediaX PushSilenceAudioData 的 SILENT 分支
            const int64_t now = core::MonotonicClock::NowUs();
            if (now - m_lastEmitUs >= silenceIntervalUs) {
                EmitSilence(silenceFrames);
                m_lastEmitUs += silenceIntervalUs;
                if (now - m_lastEmitUs > silenceIntervalUs * 10) {
                    // 累积滞后太多（线程调度抖动）直接对齐 now，避免追帧风暴
                    m_lastEmitUs = now;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
        }

        BYTE* data = nullptr;
        UINT32 framesAvailable = 0;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        hr = m_captureClient->GetBuffer(&data, &framesAvailable, &flags, &devicePosition, &qpcPosition);
        if (FAILED(hr)) {
            TG_LOGW("WinAudioCapture: GetBuffer 失败 hr=0x%08X，转静音兜底", static_cast<unsigned>(hr));
            break;
        }

        // AUDCLNT_BUFFERFLAGS_SILENT / DATA_DISCONTINUITY：WASAPI 没有真实样本可读，
        // 对齐 MediaX 直接推静音，不参与重采样（有些驱动此时 data=nullptr）
        if (framesAvailable > 0 && (flags & (AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))) {
            EmitSilence(static_cast<int>(framesAvailable));
            m_lastEmitUs = core::MonotonicClock::NowUs();
            m_captureClient->ReleaseBuffer(framesAvailable);
            continue;
        }

        if (framesAvailable > 0 && data) {
            if (m_muted.load()) {
                memset(data, 0, static_cast<size_t>(framesAvailable) * m_deviceChannels *
                                     (m_deviceFloat ? sizeof(float) : sizeof(int16_t)));
            }
            auto frame = std::make_shared<core::Frame>();
            Resample(data, static_cast<int>(framesAvailable), frame);
            if (frame->audioData) {
                frame->captureUs = core::MonotonicClock::NowUs();
                frame->ptsUs = frame->captureUs;
                m_lastEmitUs = frame->captureUs;
                Emit(std::move(frame));
            }
        }
        m_captureClient->ReleaseBuffer(framesAvailable);
    }

    SilenceLoop(token);
}

// 推一帧 S16 静音：对齐 MediaX::WinMMCapturer::PushSilenceAudioData 的下游语义
void WinAudioCapture::EmitSilence(int frames)
{
    if (frames <= 0 || m_sampleRate <= 0 || m_channels <= 0) return;
    const size_t bytes = static_cast<size_t>(frames) * static_cast<size_t>(m_channels) * sizeof(int16_t);
    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Audio;
    frame->sampleRate = m_sampleRate;
    frame->channels = m_channels;
    frame->sampleFmt = AV_SAMPLE_FMT_S16;
    frame->nbSamples = frames;
    frame->audioBytes = bytes;
    frame->audioData = std::shared_ptr<uint8_t>(new uint8_t[bytes](), std::default_delete<uint8_t[]>());
    frame->captureUs = core::MonotonicClock::NowUs();
    frame->ptsUs = frame->captureUs;
    Emit(std::move(frame));
}

// 设备不可用/无数据时持续出静音：保证音频时间轴连续，
// 否则视频会因为"音频停滞"被 clamp，封装也会卡在交织等待上。
void WinAudioCapture::SilenceLoop(std::stop_token token)
{
    const int frames = m_sampleRate > 0 ? m_sampleRate / 100 : 441;   // 10ms
    const size_t bytes = static_cast<size_t>(frames) * static_cast<size_t>(m_channels) * sizeof(int16_t);
    while (!token.stop_requested()) {
        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Audio;
        frame->sampleRate = m_sampleRate;
        frame->channels = m_channels;
        frame->sampleFmt = AV_SAMPLE_FMT_S16;
        frame->nbSamples = frames;
        frame->audioBytes = bytes;
        frame->audioData = std::shared_ptr<uint8_t>(new uint8_t[bytes](), std::default_delete<uint8_t[]>());
        frame->ptsUs = core::MonotonicClock::NowUs();
        Emit(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void WinAudioCapture::OnThreadExit()
{
    TG_LOGI("WinAudioCapture: 线程退出 loopback=%d emitted=%llu %dHz(dev=%dHz) %dch(dev=%dch) muted=%d",
            m_loopback ? 1 : 0,
            static_cast<unsigned long long>(m_emittedFrames.load()),
            m_sampleRate, m_deviceSampleRate, m_channels, m_deviceChannels,
            m_muted.load() ? 1 : 0);
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
