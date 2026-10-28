// Android 屏幕采集（系统能力：MediaProjection + VirtualDisplay + ImageReader）
//
// MediaProjection 必须由 App 侧弹系统授权框拿到（SDK 不能自己申请），拿到后通过
// ITGCapturer::SetPrivateData("android.MediaProjection", jobject) 传进来。
// 帧路径：VirtualDisplay -> ImageReader(RGBA_8888) -> libyuv 转 NV12 -> Emit。
// （VirtualDisplay 的生产者只输出 RGBA：ImageReader 配 YUV_420_888 时格式不匹配，一帧都收不到）
// 若上层绑定了编码器输入 Surface（SetEncoderSurface），则由 Java 侧直接渲染进 Surface，
// C++ 侧不搬运像素（零拷贝，见 AndroidVideoSource 的用法）。
#ifndef TGCAPTURE_PLATFORM_ANDROID_SCREEN_CAPTURE_H
#define TGCAPTURE_PLATFORM_ANDROID_SCREEN_CAPTURE_H

#ifdef __ANDROID__

#include <jni.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class AndroidScreenCapture : public capture::VideoSourceBase
{
public:
    explicit AndroidScreenCapture(VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0)
        : capture::VideoSourceBase(TypeOfSelfScreen, strategy) {}
    ~AndroidScreenCapture() override;

    int Open(int width, int height, float fps) override;

    // MediaProjection（jobject 全局引用，不拥有生命周期，由 App 持有）
    void SetProjection(jobject projection) noexcept;

    // 零拷贝：把帧直接投进编码器 Surface（MediaCodec 的输入 Surface）
    void SetEncoderSurface(jobject surface) noexcept;

    // 停掉 Java 侧采集。幂等；Close 之后再 Open 会重新 StartJava。
    // 关键：Java 线程才是真正的产帧方，Worker::Stop 只停了本地保活线程 ——
    // 不在这里切断，Stop 之后 Java 侧仍会继续回调进来。
    void Close() override;

    // 由 Java 侧回调（TGCaptureSystemCapture.nativeOnVideoFrame，YUV 三平面/半平面）
    void OnFrame(const uint8_t* y, int yStride,
                 const uint8_t* u, int uStride,
                 const uint8_t* v, int vStride,
                 int uvPixelStride, int width, int height, int64_t ptsUs);

    // 由 Java 侧回调（TGCaptureSystemCapture.nativeOnRgbaFrame，ImageReader RGBA_8888）
    void OnRgbaFrame(const uint8_t* rgba, int rowStride, int width, int height, int64_t ptsUs);

protected:
    void Loop(std::stop_token token) override;

private:
    bool StartJava();
    void StopJava();

    jobject m_projection = nullptr;
    jobject m_encoderSurface = nullptr;
    bool m_started = false;
};

}  // namespace TGCapture::platform

#endif  // __ANDROID__
#endif  // TGCAPTURE_PLATFORM_ANDROID_SCREEN_CAPTURE_H
