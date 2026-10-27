// Windows 音频采集：WASAPI（麦克风 / 扬声器 loopback），输出统一为交错 S16
#ifndef TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H
#define TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "capture/Source.h"

struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
struct ISimpleAudioVolume;

namespace TGCapture::platform
{

// 系统音频设备（WASAPI 端点）：id 可用于 WinAudioCapture::SetDeviceId 精确指定设备
struct AudioDeviceInfo
{
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

// capture=true 枚举麦克风（eCapture），false 枚举扬声器（eRender）
std::vector<AudioDeviceInfo> ListAudioDevices(bool capture);

}  // namespace TGCapture::platform

extern "C"
{
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace TGCapture::platform
{

class WinAudioCapture : public capture::AudioSourceBase
{
public:
    // loopback=true 采集扬声器（回声），false 采集麦克风
    explicit WinAudioCapture(AudioCapSourceType type, bool loopback);
    ~WinAudioCapture() override;

    int Open(int sampleRate, int channels) override;
    // 释放 WASAPI 端点与 COM。幂等；Close 之后再 Open 会重新走完整的 InitWASAPI。
    // 没有它的话设备只在析构时才放，宿主 Stop 后设备仍被占用。
    void Close() override;
    void SetVolume(float gain);
    void SetMuted(bool muted) noexcept { m_muted.store(muted); }

    // 指定端点设备（ListAudioDevices 取到的 id）；空 = 系统默认设备
    void SetDeviceId(const std::wstring& endpointId) { m_deviceId = endpointId; }
    [[nodiscard]] const std::wstring& DeviceId() const noexcept { return m_deviceId; }

protected:
    void Loop(std::stop_token token) override;
    void OnThreadExit() override;

private:
    void SilenceLoop(std::stop_token token);   // 设备不可用时的静音兜底
    void EmitSilence(int frames);              // 推一帧静音（心跳），对齐 MediaX PushSilenceAudioData

private:
    bool InitWASAPI();
    void ReleaseWASAPI();
    // 释放端点相关对象（audioClient / captureClient / device / swr），保留 enumerator 与 COM 初始化。
    // InitWASAPI 的失败分支与 RetryOpen 共用，避免两处各写一遍、各漏一点。
    void ReleaseEndpoint();
    int  RetryOpen();                           // 对齐 MediaX::WinMMCapturer::RetryOpen，仅工作线程调用
    void RegisterDeviceNotify();                // 注册 IMMNotificationClient
    void UnregisterDeviceNotify();
    void Resample(const uint8_t* src, int frames, core::FramePtr& out);

private:
    bool m_loopback = false;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    IMMDevice* m_device = nullptr;
    IMMDeviceEnumerator* m_enumerator = nullptr;     // 保留以便反注册 notification client
    void* m_notifyClient = nullptr;                   // 实际类型为内部 DeviceNotifyClient*（避免头文件暴露 IMMNotificationClient）
    UINT32 m_bufferFrames = 0;
    int m_deviceChannels = 2;
    int m_deviceSampleRate = 44100;
    bool m_deviceFloat = false;

    // 设备变更延迟标志：COM 回调线程只置标志，工作线程循环开头读到后在安全时机 RetryOpen，
    // 避免 COM 线程与采集线程并发操作 m_audioClient 导致 UAF（对齐 MediaX P0 修复）
    std::atomic<bool> m_needRetryOpen{false};

    // loopback 心跳：WASAPI loopback 在无播放时 packetLength 持续为 0，
    // 若不发任何帧，混音器会判断该通道断流 → 音频时间轴卡住。
    // 每 10ms 发一帧静音（对齐 MediaX::WinMMCapturer::PushSilenceAudioData 的 AUDCLNT_BUFFERFLAGS_SILENT 分支）
    int64_t m_lastEmitUs = 0;

    SwrContext* m_swr = nullptr;
    bool m_comInited = false;
    std::wstring m_deviceId;
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H
