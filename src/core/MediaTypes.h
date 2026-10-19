// 媒体类型描述（与既有 CVideoMediaType / CAudioMediaType 语义一致）
#ifndef TGCAPTURE_CORE_MEDIA_TYPES_H
#define TGCAPTURE_CORE_MEDIA_TYPES_H

#include <string>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace TGCapture::core
{

class VideoMediaType
{
public:
    void SetWidth(int w) noexcept { m_width = w; }
    void SetHeight(int h) noexcept { m_height = h; }
    void SetPixelFormat(AVPixelFormat fmt) noexcept { m_pixFmt = fmt; }
    void SetFrameRate(int num, int den) noexcept { m_frameRate = {num, den}; }

    [[nodiscard]] int Width() const noexcept { return m_width; }
    [[nodiscard]] int Height() const noexcept { return m_height; }
    [[nodiscard]] AVPixelFormat PixelFormat() const noexcept { return m_pixFmt; }
    [[nodiscard]] AVRational FrameRate() const noexcept { return m_frameRate; }
    [[nodiscard]] double FrameRateFloat() const noexcept
    {
        return m_frameRate.den > 0 ? static_cast<double>(m_frameRate.num) / m_frameRate.den : 0.0;
    }
    [[nodiscard]] int64_t FrameIntervalUs() const noexcept
    {
        const double fps = FrameRateFloat();
        return fps > 0 ? static_cast<int64_t>(1000000.0 / fps) : 0;
    }
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_width > 0 && m_height > 0 && m_pixFmt != AV_PIX_FMT_NONE && m_frameRate.den > 0;
    }
    [[nodiscard]] std::string Info() const;

private:
    int m_width = 0;
    int m_height = 0;
    AVPixelFormat m_pixFmt = AV_PIX_FMT_NONE;
    AVRational m_frameRate{30, 1};
};

class AudioMediaType
{
public:
    void SetSampleRate(int rate) noexcept { m_sampleRate = rate; }
    void SetChannels(int channels) noexcept { m_channels = channels; }
    void SetSampleFormat(AVSampleFormat fmt) noexcept { m_sampleFmt = fmt; }
    void SetFrameSize(int size) noexcept { m_frameSize = size; }

    [[nodiscard]] int SampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] int Channels() const noexcept { return m_channels; }
    [[nodiscard]] AVSampleFormat SampleFormat() const noexcept { return m_sampleFmt; }
    [[nodiscard]] int FrameSize() const noexcept { return m_frameSize; }
    [[nodiscard]] int BytesPerSample() const noexcept { return av_get_bytes_per_sample(m_sampleFmt); }
    [[nodiscard]] bool IsPlanar() const noexcept { return av_sample_fmt_is_planar(m_sampleFmt) != 0; }
    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_sampleRate > 0 && m_channels > 0 && m_sampleFmt != AV_SAMPLE_FMT_NONE;
    }
    [[nodiscard]] std::string Info() const;

private:
    int m_sampleRate = 0;
    int m_channels = 0;
    AVSampleFormat m_sampleFmt = AV_SAMPLE_FMT_NONE;
    int m_frameSize = 1024;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_MEDIA_TYPES_H
