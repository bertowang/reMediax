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
    Close();
}

void IOSScreenCapture::Close()
{
    if (!m_recorder) {
        m_started = false;
        return;
    }
    RPScreenRecorder* recorder = (__bridge_transfer RPScreenRecorder*)m_recorder;
    m_recorder = nullptr;
    if (@available(iOS 11.0, *)) {
        // stopCaptureWithHandler 是**异步**的：只发一个停止请求就返回，
        // 在飞的采样回调仍可能在之后到达。这里等它确认停止，避免回调落在已销毁的对象上。
        // 带超时是因为 completionHandler 走主队列，若本函数就在主线程被调用会互等 ——
        // 超时退出后仍有基类 EmitGate 兜底（回调会被丢弃，不会进下游）。
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [recorder stopCaptureWithHandler:^(NSError* error) {
            if (error) {
                TG_LOGW("IOSScreenCapture: stopCapture 返回错误: %s",
                        [[error localizedDescription] UTF8String]);
            }
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3LL * NSEC_PER_SEC));
    }
    m_started = false;
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

    // 重复 Open 先拆上一轮：否则下面的 __bridge_retained 覆盖旧指针，每次漏一个 retain
    Close();

    RPScreenRecorder* recorder = [RPScreenRecorder sharedRecorder];
    m_recorder = (__bridge_retained void*)recorder;

    // 不用 __weak：它对 C++ 裸指针没有弱引用语义（写了也永远非空）。
    // 生命周期保证来自 Close() 的同步停止 + 基类 Emit 的 EmitGate 栅栏。
    auto* self = this;
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
            self->OnSampleBuffer(pixelBuffer, ptsUs);
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
    Close();   // 上面已经 retain 了 recorder，这条失败路径原先直接漏掉
    return -2;
}

void IOSScreenCapture::Loop(std::stop_token token)
{
    // 帧由 ReplayKit 回调驱动，这里只保活。
    // 不在这里 stopCapture：recorder 的启停只归 Open/Close 管（Close 带完成回调同步等待）。
    // 原先这里调 stopCaptureWithHandler:nil —— ReplayKit 会无条件调用 handler，
    // nil block 直接 SIGSEGV（iOS 模拟器实测 KERN_INVALID_ADDRESS 0x10），且与 Close 重复停止。
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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
