// iOS 采集源：
//  - 硬编直连：CVPixelBuffer -> VideoToolbox 编码器，AVFrame 承载 CVPixelBufferRef，
//    CPU 不做像素搬运。
//  - 软编兜底：CVPixelBuffer 锁基址后按 I420/NV12 拷贝一份送 openh264。
#ifndef TGCAPTURE_PLATFORM_IOS_VIDEO_SOURCE_H
#define TGCAPTURE_PLATFORM_IOS_VIDEO_SOURCE_H

#include <CoreVideo/CoreVideo.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class IOSVideoSource : public capture::VideoSourceBase
{
public:
    explicit IOSVideoSource(VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0)
        : capture::VideoSourceBase(TypeOfGameEngine4Slave, strategy) {}

    int Open(int width, int height, float fps) override
    {
        m_width = width;
        m_height = height;
        m_fps = fps > 0 ? fps : 30.0f;
        return 0;
    }

    // 硬编直连：直接把 CVPixelBuffer 交给编码器（VideoToolbox）
    void PushPixelBuffer(CVPixelBufferRef pixelBuffer, int64_t ptsUs, bool zeroCopy = true)
    {
        if (!pixelBuffer) {
            return;
        }
        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Video;
        frame->width = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
        frame->height = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
        frame->ptsUs = ptsUs;
        frame->captureUs = core::MonotonicClock::NowUs();

        if (zeroCopy) {
            CFRetain(pixelBuffer);
            frame->hwKind = core::HwKind::CVPixelBuffer;
            frame->hwHandle = pixelBuffer;
            frame->hwOwner = std::shared_ptr<void>(const_cast<void*>(reinterpret_cast<const void*>(pixelBuffer)),
                                                  [](void* p) {
                                                      if (p) {
                                                          CFRelease(p);
                                                      }
                                                  });
        } else {
            CopyToSystemMemory(pixelBuffer, frame);
        }
        Emit(std::move(frame));
    }

protected:
    void Loop(std::stop_token token) override
    {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    static void CopyToSystemMemory(CVPixelBufferRef pixelBuffer, const core::FramePtr& frame)
    {
        CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        const OSType type = CVPixelBufferGetPixelFormatType(pixelBuffer);
        const size_t planeCount = CVPixelBufferGetPlaneCount(pixelBuffer);

        if (planeCount >= 2) {
            const size_t ySize = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0) *
                                 CVPixelBufferGetHeightOfPlane(pixelBuffer, 0);
            const size_t uvSize = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1) *
                                  CVPixelBufferGetHeightOfPlane(pixelBuffer, 1);
            auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize], std::default_delete<uint8_t[]>());
            memcpy(buffer.get(), CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0), ySize);
            memcpy(buffer.get() + ySize, CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1), uvSize);
            frame->planes[0] = buffer;
            frame->strides[0] = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0));
            frame->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
            frame->strides[1] = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1));
            frame->pixFmt = (type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                             type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                                ? AV_PIX_FMT_NV12
                                : AV_PIX_FMT_NV12;
        }
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    }
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_IOS_VIDEO_SOURCE_H
