#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <ReplayKit/ReplayKit.h>
#import <UIKit/UIKit.h>

// AVFoundation 有 `typedef NSString* AVMediaType`，ffmpeg 有 `enum AVMediaType`，
// 同一个 .mm 里同时出现会报 definition of type 'AVMediaType' conflicts with typedef。
// 只在包含工程头（会带进 ffmpeg 头）的这段时间内把 ffmpeg 侧的枚举改名，随后立即 #undef。
#define AVMediaType TG_AVMediaType
#include "core/Clock.h"
#include "core/Logger.h"
#include "platform/ios/IOSScreenCapture.h"
#undef AVMediaType

namespace TGCapture::platform
{

IOSScreenCapture::~IOSScreenCapture()
{
    Stop();
    if (m_recorder) {
        RPScreenRecorder* recorder = (__bridge_transfer RPScreenRecorder*)m_recorder;
        (void)recorder;
        m_recorder = nullptr;
    }
}

int IOSScreenCapture::Open(int width, int height, float fps)
{
    m_width = width > 0 ? width : 1280;
    m_height = height > 0 ? height : 720;
    m_fps = fps > 0 ? fps : 30.0f;

    if (![RPScreenRecorder class]) {
        TG_LOGE("IOSScreenCapture: ReplayKit 不可用");
        return -1;
    }

    RPScreenRecorder* recorder = [RPScreenRecorder sharedRecorder];
    m_recorder = (__bridge_retained void*)recorder;

    __weak auto weakSelf = this;
    if (@available(iOS 11.0, *)) {
        [recorder startCaptureWithHandler:^(CMSampleBufferRef sampleBuffer,
                                            RPSampleBufferType bufferType, NSError* error) {
            if (error || bufferType != RPSampleBufferTypeVideo) {
                return;
            }
            CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
            if (!pixelBuffer) {
                return;
            }
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
            const int64_t ptsUs = (pts.timescale > 0)
                                      ? (pts.value * 1000000LL / pts.timescale)
                                      : core::MonotonicClock::NowUs();
            if (auto* self = weakSelf) {
                self->OnSampleBuffer(pixelBuffer, ptsUs);
            }
        }
            completionHandler:^(NSError* error) {
              if (error) {
                  TG_LOGE("IOSScreenCapture: startCapture 失败: %s",
                          [[error localizedDescription] UTF8String]);
              } else {
                  TG_LOGI("IOSScreenCapture: ReplayKit 就绪 %dx%d fps=%.1f", m_width, m_height, m_fps);
              }
            }];
        m_started = true;
        return 0;
    }

    TG_LOGE("IOSScreenCapture: 需要 iOS 11+ 的 startCaptureWithHandler");
    return -2;
}

void IOSScreenCapture::Loop(std::stop_token token)
{
    // 帧由 ReplayKit 回调驱动，这里只保活
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (@available(iOS 11.0, *)) {
        if (m_recorder) {
            [(__bridge RPScreenRecorder*)m_recorder stopCaptureWithHandler:nil];
        }
    }
    m_started = false;
}

void IOSScreenCapture::OnSampleBuffer(void* pixelBuffer, int64_t ptsUs)
{
    CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(pixelBuffer);
    if (!pb) {
        return;
    }

    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Video;
    frame->width = static_cast<int>(CVPixelBufferGetWidth(pb));
    frame->height = static_cast<int>(CVPixelBufferGetHeight(pb));
    frame->ptsUs = ptsUs;
    frame->captureUs = core::MonotonicClock::NowUs();

    CFRetain(pb);
    frame->hwKind = core::HwKind::CVPixelBuffer;   // 零拷贝：直接喂 VideoToolbox
    frame->hwHandle = pb;
    frame->hwOwner = std::shared_ptr<void>(pb, [](void* p) {
        if (p) {
            CFRelease(p);
        }
    });
    Emit(std::move(frame));
}

}  // namespace TGCapture::platform

#endif  // __APPLE__
