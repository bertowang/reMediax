// iOS 屏幕采集（系统能力：ReplayKit）
//
// 说明（iOS 的系统限制，不是实现缺失）：
//  - RPScreenRecorder 只能采集**本 App** 的画面；采集全系统需要额外的
//    Broadcast Upload Extension（App Group 传帧），那部分必须由宿主 App 提供。
//  - iOS 没有"窗口"概念，窗口采集不可用（ITGCapturer::IsSupported(Window) 返回 false）。
//  - iOS 无法内录系统声音（扬声器）：需要 ReplayKit 扩展才拿得到，App 内不可得。
//
// 帧路径：RPScreenRecorder 回调 CVPixelBuffer -> 零拷贝交给 VideoToolbox（hwKind=CVPixelBuffer）
#ifndef TGCAPTURE_PLATFORM_IOS_SCREEN_CAPTURE_H
#define TGCAPTURE_PLATFORM_IOS_SCREEN_CAPTURE_H

#ifdef __APPLE__

#include <CoreVideo/CoreVideo.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class IOSScreenCapture : public capture::VideoSourceBase
{
public:
    explicit IOSScreenCapture(VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0)
        : capture::VideoSourceBase(TypeOfSelfScreen, strategy) {}
    ~IOSScreenCapture() override;

    int Open(int width, int height, float fps) override;
    // 停止 ReplayKit 并释放 recorder。幂等；Close 之后再 Open 会重新 startCapture。
    void Close() override;

    // 由 ObjC++ 实现侧回调
    void OnSampleBuffer(void* pixelBuffer, int64_t ptsUs);

protected:
    void Loop(std::stop_token token) override;

private:
    void* m_recorder = nullptr;   // RPScreenRecorder*（ObjC 对象，ARC 由实现文件管理）
    bool m_started = false;
};

}  // namespace TGCapture::platform

#endif  // __APPLE__
#endif  // TGCAPTURE_PLATFORM_IOS_SCREEN_CAPTURE_H
