// Windows 音频采集：WASAPI（麦克风 / 扬声器 loopback），输出统一为交错 S16
#ifndef TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H
#define TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H

#include <windows.h>

#include <memory>

#include "capture/Source.h"

struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
struct ISimpleAudioVolume;

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

protected:
    void Loop(std::stop_token token) override;
    void OnThreadExit() override;

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
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_WIN_WIN_AUDIO_CAPTURE_H
