// 采集源抽象：平台实现继承后按帧产出，帧以 shared_ptr 交给总线
#ifndef TGCAPTURE_CAPTURE_SOURCE_H
#define TGCAPTURE_CAPTURE_SOURCE_H

#include <atomic>
#include <functional>
#include <memory>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Frame.h"
#include "core/Thread.h"

namespace TGCapture::capture
{

// 视频采集源：平台实现周期性调用 Emit(frame)
class VideoSourceBase : public IVideoSource, public core::Worker
{
public:
    VideoSourceBase(VideoCapSourceType type, VideoScaleStrategy strategy)
        : IVideoSource(type, strategy), core::Worker("video_capture") {}

    using EmitCallback = std::function<void(core::FramePtr)>;

    void SetEmitCallback(EmitCallback cb) { m_emit = std::move(cb); }

    // 打开/关闭由平台实现
    virtual int Open(int width, int height, float fps) = 0;

    void StopCapture()
    {
        Stop();
    }

    [[nodiscard]] bool IsCapturing() const noexcept { return IsRunning(); }

    // 采集实际输出的尺寸（Open 之后有效）
    [[nodiscard]] int FrameWidth() const noexcept { return m_width; }
    [[nodiscard]] int FrameHeight() const noexcept { return m_height; }

protected:
    void Emit(core::FramePtr frame)
    {
        if (m_emit && frame) {
            m_emit(std::move(frame));
        }
    }

    int m_width = 0;
    int m_height = 0;
    float m_fps = 30.0f;

private:
    EmitCallback m_emit;
};

// 音频采集源：产出交错 S16 PCM
class AudioSourceBase : public IAudioSource, public core::Worker
{
public:
    AudioSourceBase(AudioCapSourceType type) : IAudioSource(type), core::Worker("audio_capture") {}

    using EmitCallback = std::function<void(core::FramePtr)>;

    void SetEmitCallback(EmitCallback cb) { m_emit = std::move(cb); }

    virtual int Open(int sampleRate, int channels) = 0;

    void StopCapture() { Stop(); }

    [[nodiscard]] bool IsCapturing() const noexcept { return IsRunning(); }

protected:
    void Emit(core::FramePtr frame)
    {
        if (m_emit && frame) {
            m_emit(std::move(frame));
        }
    }

    int m_sampleRate = 44100;
    int m_channels = 2;
    std::atomic_bool m_muted{false};

private:
    EmitCallback m_emit;
};

}  // namespace TGCapture::capture

#endif  // TGCAPTURE_CAPTURE_SOURCE_H
