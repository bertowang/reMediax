// iOS 音频采集（系统能力：AVAudioSession + AVAudioEngine）
//  - 麦克风：AVAudioEngine.inputNode 的 tap，输出交错 S16 PCM
//  - 扬声器/系统内录：iOS 不支持（需 Broadcast Upload Extension），IsSupported(Speaker) 返回 false
#ifndef TGCAPTURE_PLATFORM_IOS_AUDIO_CAPTURE_H
#define TGCAPTURE_PLATFORM_IOS_AUDIO_CAPTURE_H

#ifdef __APPLE__

#include <AudioToolbox/AudioToolbox.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class IOSAudioCapture : public capture::AudioSourceBase
{
public:
    explicit IOSAudioCapture(AudioCapSourceType type);
    ~IOSAudioCapture() override;

    int Open(int sampleRate, int channels) override;

    // 由 ObjC++ 实现侧回调（交错 S16）
    void OnPcm(const int16_t* data, int frames, int64_t ptsUs);

protected:
    void Loop(std::stop_token token) override;

private:
    void* m_engine = nullptr;   // AVAudioEngine*（ARC 由实现文件管理）
    bool m_started = false;
};

}  // namespace TGCapture::platform

#endif  // __APPLE__
#endif  // TGCAPTURE_PLATFORM_IOS_AUDIO_CAPTURE_H
