// 视频编码器：封装 ffmpeg avcodec，支持软编(libopenh264)与硬编直连
// 硬编零拷贝路径：采集帧若携带 HwKind（D3D11Texture / CVPixelBuffer / AndroidSurface），
// 直接构造硬件 AVFrame 送入编码器，CPU 不参与像素搬运。
#ifndef TGCAPTURE_CODEC_VIDEO_ENCODER_H
#define TGCAPTURE_CODEC_VIDEO_ENCODER_H

#include <atomic>
#include <memory>
#include <string>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "core/Frame.h"
#include "core/MediaTypes.h"
#include "core/Queue.h"
#include "core/Thread.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace TGCapture
{
class Muxer;

namespace codec
{

class VideoEncoder : public core::BusNode, public core::Worker
{
public:
    explicit VideoEncoder(size_t queueCapacity = 12);
    ~VideoEncoder() override;

    // 打开编码器
    int Open(const VideoEncoderParams& params, const core::VideoMediaType& srcType);
    void Close();

    void SetMuxer(Muxer* muxer) noexcept { m_muxer = muxer; }
    void SetSyncClock(std::shared_ptr<core::AVSync> sync) { m_sync = std::move(sync); }
    void SetMaxLagUs(int64_t us) noexcept { m_maxLagUs = us; }

    // BusNode
    bool Submit(core::FramePtr frame) override;
    size_t Pending() const override;

    // 背压：下游（muxer）积压过多时上游可减速
    size_t DownstreamPending() const;

    [[nodiscard]] AVCodecContext* CodecContext() const noexcept { return m_ctx; }
    [[nodiscard]] bool IsOpened() const noexcept { return m_ctx != nullptr; }
    [[nodiscard]] uint64_t EncodedFrames() const noexcept { return m_encoded.load(); }
    [[nodiscard]] uint64_t DroppedFrames() const noexcept { return m_dropped.load(); }
    [[nodiscard]] const std::string& CodecName() const noexcept { return m_codecName; }

protected:
    void Loop(std::stop_token token) override;
    void OnThreadExit() override;

private:
    int EncodeOne(const core::FramePtr& frame);
    int DrainLocked();
    void DeliverPacket(core::AVPacketPtr packet);

    // 把内部帧转成 AVFrame（系统内存或硬件直连）
    core::AVFramePtr BuildAVFrame(const core::FramePtr& frame);
    bool InitHardwareContext(AVPixelFormat srcFmt);
    // 尺寸/像素格式归一：采集分辨率与编码器不一致时用 sws_scale 转换
    bool EnsureSwsContext(int srcWidth, int srcHeight, AVPixelFormat srcFmt);

private:
    std::string m_codecName;
    AVCodecContext* m_ctx = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVBufferRef* m_hwFramesCtx = nullptr;
    SwsContext* m_sws = nullptr;
    int m_swsSrcWidth = 0;
    int m_swsSrcHeight = 0;
    AVPixelFormat m_swsSrcFmt = AV_PIX_FMT_NONE;
    AVPixelFormat m_swPixFmt = AV_PIX_FMT_NONE;
    bool m_useHwFrame = false;

    core::VideoMediaType m_srcType;
    core::VideoMediaType m_dstType;
    int m_gop = 15;

    std::unique_ptr<core::BoundedQueue<core::FramePtr>> m_queue;
    std::shared_ptr<core::AVSync> m_sync;
    int64_t m_maxLagUs = 120000;  // 视频落后音频超过 120ms 则丢帧

    Muxer* m_muxer = nullptr;  // 非拥有

    std::atomic<uint64_t> m_encoded{0};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic_bool m_opened{false};
};

}  // namespace codec
}  // namespace TGCapture

#endif  // TGCAPTURE_CODEC_VIDEO_ENCODER_H
