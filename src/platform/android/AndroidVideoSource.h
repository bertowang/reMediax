// Android 采集源：
//  - 硬编直连：采集 Surface 直接作为编码器（MediaCodec/h264_mediacodec）输入，
//    每帧流转 采集 Surface -> 编码器，CPU 不做像素搬运。
//  - 软编兜底：外部 push 的 ByteBuffer（NV12/I420）经 libyuv 归一化后送 openh264。
#ifndef TGCAPTURE_PLATFORM_ANDROID_VIDEO_SOURCE_H
#define TGCAPTURE_PLATFORM_ANDROID_VIDEO_SOURCE_H

#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class AndroidVideoSource : public capture::VideoSourceBase
{
public:
    explicit AndroidVideoSource(VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0)
        : capture::VideoSourceBase(TypeOfGameEngine4Slave, strategy) {}

    int Open(int width, int height, float fps) override
    {
        m_width = width;
        m_height = height;
        m_fps = fps > 0 ? fps : 30.0f;
        return 0;
    }

    // 硬编直连：绑定编码器输入 Surface（由 MediaCodec 创建后传给采集侧）
    void SetEncoderSurface(ANativeWindow* window) noexcept { m_encoderSurface = window; }
    [[nodiscard]] ANativeWindow* EncoderSurface() const noexcept { return m_encoderSurface; }

    // 采集侧每帧渲染到该 Surface（OpenGL/Vulkan/Camera2 均可），CPU 零拷贝
    [[nodiscard]] bool IsSurfaceDirect() const noexcept { return m_encoderSurface != nullptr; }

    // 软编兜底路径：push 系统内存帧
    void PushFrame(const uint8_t* data, int width, int height, AVPixelFormat fmt, int64_t ptsUs)
    {
        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Video;
        frame->width = width;
        frame->height = height;
        frame->pixFmt = fmt;
        frame->ptsUs = ptsUs;
        frame->captureUs = core::MonotonicClock::NowUs();
        const size_t bytes = static_cast<size_t>(width) * height * 3 / 2;
        auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
        memcpy(buffer.get(), data, bytes);
        frame->planes[0] = buffer;
        frame->strides[0] = width;
        frame->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + static_cast<size_t>(width) * height);
        frame->strides[1] = width;
        Emit(std::move(frame));
    }

protected:
    void Loop(std::stop_token token) override
    {
        // Surface 直连模式下无需本线程搬运（帧由采集端直接渲染进 Surface）
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    ANativeWindow* m_encoderSurface = nullptr;  // 不拥有
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_ANDROID_VIDEO_SOURCE_H
