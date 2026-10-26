// iOS 音频采集（系统能力：AVAudioSession + AVAudioEngine）
//  - 麦克风：AVAudioEngine.inputNode 的 tap，输出交错 S16 PCM
//  - 扬声器/系统内录：iOS 不支持（需 Broadcast Upload Extension），IsSupported(Speaker) 返回 false
#ifndef TGCAPTURE_PLATFORM_IOS_AUDIO_CAPTURE_H
#define TGCAPTURE_PLATFORM_IOS_AUDIO_CAPTURE_H

#ifdef __APPLE__

#include <atomic>

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

    // AVAudioSession 中断/路由外推 API（由内部 observer 对象在主线程调用）：
    //   NotifyAudioInterrupted(true/false)   —— 系统中断（电话、Siri 等）开始/结束
    //   NotifyAudioRouteChanged(reason)      —— 路由变更（耳机插拔、设备不可用）
    // 中断/路由异常期间 engine 的 tap 不会被触发，Loop 转为静音心跳保持音频时间轴连续。
    void NotifyAudioInterrupted(bool begin, bool shouldResume);
    void NotifyAudioRouteChanged(int reason);

protected:
    void Loop(std::stop_token token) override;

private:
    bool RestartEngine();   // 中断结束或路由恢复后尝试重启

private:
    void* m_engine = nullptr;      // AVAudioEngine*（ARC 由实现文件管理）
    void* m_observer = nullptr;    // TGIOSAudioObserver*（ARC retained）
    bool  m_started = false;

    // 中断/路由异常标志：Loop 据此判定是否推静音心跳
    std::atomic<bool> m_interrupted{false};
};

}  // namespace TGCapture::platform

#endif  // __APPLE__
#endif  // TGCAPTURE_PLATFORM_IOS_AUDIO_CAPTURE_H
