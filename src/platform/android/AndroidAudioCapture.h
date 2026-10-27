// Android 音频采集（系统能力，不依赖 GVoice）
//  - 麦克风：AudioRecord(MediaRecorder.AudioSource.MIC)
//  - 扬声器/系统内录：AudioPlaybackCapture（Android 10 / API 29+，需 MediaProjection 授权）
// 两者都产出交错 S16 PCM，由 TGCaptureSystemCapture.nativeOnPcm 回调进内核。
#ifndef TGCAPTURE_PLATFORM_ANDROID_AUDIO_CAPTURE_H
#define TGCAPTURE_PLATFORM_ANDROID_AUDIO_CAPTURE_H

#ifdef __ANDROID__

#include <jni.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class AndroidAudioCapture : public capture::AudioSourceBase
{
public:
    // loopback=false 麦克风；true 系统内录
    AndroidAudioCapture(AudioCapSourceType type, bool loopback);
    ~AndroidAudioCapture() override;

    int Open(int sampleRate, int channels) override;

    void SetProjection(jobject projection) noexcept { m_projection = projection; }

    // 停掉 Java 侧采集。幂等；Close 之后再 Open 会重新 StartJava。
    // 关键：Java 线程才是真正的产帧方，Worker::Stop 只停了本地保活线程 ——
    // 不在这里切断，Stop 之后 Java 侧仍会继续回调进来。
    void Close() override;

    // 由 Java 侧回调
    void OnPcm(const uint8_t* data, int bytes, int64_t ptsUs);

protected:
    void Loop(std::stop_token token) override;

private:
    bool StartJava();
    void StopJava();

    bool m_loopback = false;
    jobject m_projection = nullptr;
    bool m_started = false;
};

}  // namespace TGCapture::platform

#endif  // __ANDROID__
#endif  // TGCAPTURE_PLATFORM_ANDROID_AUDIO_CAPTURE_H
