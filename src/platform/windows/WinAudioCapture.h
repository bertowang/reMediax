// Windows 音频采集：WASAPI（麦克风 / 扬声器 loopback），输出统一为交错 S16
#ifndef TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H
#define TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "capture/Source.h"

struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
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

private:
    bool InitWASAPI();
    void ReleaseWASAPI();
    void Resample(const uint8_t* src, int frames, core::FramePtr& out);

private:
    bool m_loopback = false;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    IMMDevice* m_device = nullptr;
    UINT32 m_bufferFrames = 0;
    int m_deviceChannels = 2;
    int m_deviceSampleRate = 44100;
    bool m_deviceFloat = false;

    SwrContext* m_swr = nullptr;
    bool m_comInited = false;
    std::wstring m_deviceId;
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H
