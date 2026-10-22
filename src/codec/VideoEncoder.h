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
#include "core/VideoTransform.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
#endif

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

    // 性能模式：Gpu 时采集帧留在显存直送硬编；不可用时自动回落 Cpu
    void SetPerformanceMode(PerformanceMode mode) noexcept { m_perfMode = mode; }
    PerformanceMode GetPerformanceMode() const noexcept { return m_perfMode; }
    // Windows：复用采集侧的 D3D11 设备（同设备才能做显存内拷贝，避免跨设备/回读）
    void SetExternalDevice(void* device) noexcept { m_externalDevice = device; }
    bool IsGpuPath() const noexcept { return m_useHwFrame && m_perfMode == PerformanceMode::Gpu; }

    // 画面变换（采集帧 → 编码分辨率之前应用）。默认不做变换。
    void SetVideoTransform(const core::VideoTransform& transform) { m_transform = transform; }
    const core::VideoTransform& GetVideoTransform() const noexcept { return m_transform; }
    // GPU 模式下，需要变换或分辨率不一致时是否允许回读到内存处理
    void SetGpuReadbackFallback(bool allow) noexcept { m_gpuReadbackFallback = allow; }

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
    bool InitHardwareFrames(AVPixelFormat hwFmt);
#ifdef _WIN32
    bool CopyFromD3D11Texture(const core::FramePtr& frame, AVFrame* dst);
    // GPU 帧需要变换或分辨率不一致时，回读到系统内存再交给归一化层
    bool ReadBackD3D11Texture(const core::FramePtr& frame, core::FramePtr& out);
    bool EnsureStagingTexture(int width, int height);
#endif
    // 尺寸/像素格式归一：采集分辨率与编码器不一致时用 sws_scale 转换
    bool EnsureSwsContext(int srcWidth, int srcHeight, AVPixelFormat srcFmt);
    // 统一的画面归一化入口：应用变换 + 缩放 + 转格式；失败时返回原帧
    core::FramePtr NormalizeFrame(const core::FramePtr& frame);
    // 系统内存帧 → AVFrame（sws_scale 缩放到编码器尺寸并转格式）
    bool FillFromCpuFrame(const core::FramePtr& src, core::AVFramePtr& avFrame);

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

    PerformanceMode m_perfMode = PerformanceMode::Cpu;
    void* m_externalDevice = nullptr;   // Windows: ID3D11Device*（采集侧设备）
#ifdef _WIN32
    ID3D11DeviceContext* m_d3dContext = nullptr;   // 采集设备的 context，用于显存内拷贝
    // GPU 回读用的暂存纹理（按需创建，按尺寸缓存）
    ID3D11Texture2D* m_stagingTex = nullptr;
    int m_stagingWidth = 0;
    int m_stagingHeight = 0;
#endif

    // 画面变换配置（默认恒等，不做额外处理）
    core::VideoTransform m_transform;
    bool m_gpuReadbackFallback = true;

    core::VideoMediaType m_srcType;
    core::VideoMediaType m_dstType;
    int m_gop = 15;

    std::unique_ptr<core::BoundedQueue<core::FramePtr>> m_queue;
    std::shared_ptr<core::AVSync> m_sync;
    int64_t m_maxLagUs = 120000;  // 视频落后音频超过 120ms 则丢帧

    Muxer* m_muxer = nullptr;  // 非拥有

    std::atomic<uint64_t> m_encoded{0};
    std::atomic<uint64_t> m_dropped{0};
    int64_t m_lastPtsUs = -1;   // 上一帧 PTS，用于 PTS 回退保护
    std::atomic_bool m_opened{false};
};

}  // namespace codec
}  // namespace TGCapture

#endif  // TGCAPTURE_CODEC_VIDEO_ENCODER_H
