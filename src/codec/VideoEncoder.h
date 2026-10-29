// 视频编码器：封装 ffmpeg avcodec，支持软编(libopenh264)与硬编直连
// 硬编零拷贝路径：采集帧若携带 HwKind（D3D11Texture / CVPixelBuffer / AndroidSurface），
// 直接构造硬件 AVFrame 送入编码器，CPU 不参与像素搬运。
#ifndef TGCAPTURE_CODEC_VIDEO_ENCODER_H
#define TGCAPTURE_CODEC_VIDEO_ENCODER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "core/Frame.h"
#include "core/Health.h"
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
namespace codec
{

class VideoEncoder : public core::BusNode, public core::Worker
{
public:
    explicit VideoEncoder(size_t queueCapacity = 12);
    ~VideoEncoder() override;

    // 打开编码器。请求的是硬编（MediaCodec / VideoToolbox / NVENC…）但本机打不开时，
    // 自动回落 libopenh264 并返回成功 —— 实际用的编码器以 CodecName() 为准。
    // 软编本身打不开（名字写错 / 未编进 ffmpeg）不回落，照常返回失败。
    int Open(const VideoEncoderParams& params, const core::VideoMediaType& srcType);
    void Close();

    // 运行期切换分辨率 / 码率（线程安全，立即返回）：由编码线程在处理下一帧之前执行
    //   排空旧编码器 → 按新参数重开（沿用当前实际在用的编码器，不再探测 / 回落）→ 首帧强制 IDR，
    //   新的 SPS/PPS 以 AV_PKT_DATA_NEW_EXTRADATA 挂在第一个关键包上交给封装。
    // 只取 params 的 width / height / vBitrate / vBitrateMaxFactor / gop，帧率与像素格式不变。
    // 新参数打不开时回退原参数并上报 TGErr_VideoEncodeFailed；第一个新关键包送出后上报
    // EsCap_Notify_VideoMetaInfo_Changed（content = "宽x高"，data = 码率）。
    // 多次调用只保留最后一次。编码线程未运行（Prepared）时在线程启动后、编码第一帧之前生效
    void Reconfigure(const VideoEncoderParams& params);
    // 当前生效的编码尺寸（编码线程写，任意线程读）
    [[nodiscard]] int EncodeWidth() const noexcept { return m_encodeWidth.load(); }
    [[nodiscard]] int EncodeHeight() const noexcept { return m_encodeHeight.load(); }

    // ---- 总线生命周期（output 组；Open 带参数，由持有者在 Prepare 里调用）----
    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Output; }
    [[nodiscard]] bool IsActive() const override { return IsOpened(); }
    bool NodeStart() override { return Start(); }
    void NodeStop() override { Stop(); }
    void NodeClose() override { Close(); }

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

    // BusNode：编码后的包经 Forward 交给链上的下一个节点（muxer）；
    // 下游积压用基类的 DownstreamPending() 查询，用于背压
    bool Submit(core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override;
    // Flush：丢弃编码队列积压；RequestKeyFrame：下一帧强制输出 IDR
    bool OnCommand(const core::BusCommand& command) override;

    [[nodiscard]] AVCodecContext* CodecContext() const noexcept { return m_ctx; }
    // 用标志而不是 m_ctx 判空：运行期重开时编码线程会替换 m_ctx，别的线程（总线启停查 IsActive）不能读它
    [[nodiscard]] bool IsOpened() const noexcept { return m_opened.load(); }
    [[nodiscard]] uint64_t EncodedFrames() const noexcept { return m_encoded.load(); }
    [[nodiscard]] uint64_t DroppedFrames() const noexcept { return m_dropped.load(); }
    // 返回副本：运行期切换分辨率时硬编可能回落软编，编码线程会改写编码器名
    [[nodiscard]] std::string CodecName() const
    {
        std::lock_guard<std::mutex> lock(m_codecNameMutex);
        return m_codecName;
    }
    // 最后一次底层失败的 AVERROR（0 = 无）：Open 失败后由 TGCapturer 填进事件 extra
    [[nodiscard]] int32_t LastAvError() const noexcept { return m_lastAvError.load(); }

    // 端到端编码延迟 EMA（微秒）：capture → encode out 的滑动平均，供 GetStatsInfo 输出
    [[nodiscard]] int64_t EncodeLatencyUs() const noexcept { return m_encodeLatencyUs.load(); }

protected:
    void Loop(std::stop_token token) override;
    void OnThreadExit() override;

private:
    // 只建 / 拆编码器上下文及其附属（硬件帧池、sws、staging 纹理），不碰队列与时间轴状态
    int OpenCodec(const VideoEncoderParams& params, const core::VideoMediaType& srcType);
    void ReleaseCodec();
    // 编码线程上执行挂起的 Reconfigure
    void ApplyPendingReconfigure();
    int EncodeOne(const core::FramePtr& frame);
    // 编码结果归集：连续失败超过 1s 上报一次 TGErr_VideoEncodeFailed（extra = AVERROR）
    void OnEncodeResult(bool ok, int ret);
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
    std::string m_codecName;               // 写：持 m_codecNameMutex；编码线程 / Open 内读可不加锁
    mutable std::mutex m_codecNameMutex;
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

    std::atomic_bool m_forceKeyFrame{false};   // RequestKeyFrame 置位，EncodeOne 消费

    std::atomic<uint64_t> m_encoded{0};
    std::atomic<uint64_t> m_dropped{0};
    // 端到端编码延迟 EMA（微秒），EncodeOne 结束时按 α=1/8 更新（对齐 MediaX 常用 EMA 系数）
    std::atomic<int64_t> m_encodeLatencyUs{0};
    // 运行期失败判定（core/Health.h）：连续失败超过 1s 上报一次 TGErr_VideoEncodeFailed
    core::FailureTracker m_encodeFail;
    std::atomic<int32_t> m_lastAvError{0};
    int64_t m_lastPtsUs = -1;   // 上一帧 PTS，用于 PTS 回退保护
    std::atomic_bool m_opened{false};

    // ---- 运行期重开 ----
    VideoEncoderParams m_params;                 // 当前生效的参数（Open / 编码线程写）
    std::string m_preferredCodec;                // Open 时实际打开的编码器：运行期重开优先用它（硬编临时回落后可切回）
    std::mutex m_reconfMutex;
    std::unique_ptr<VideoEncoderParams> m_pendingParams;   // 受 m_reconfMutex 保护
    std::atomic_bool m_hasPending{false};
    bool m_attachExtradata = false;              // 编码线程：下一个关键包挂 NEW_EXTRADATA 并发通知
    std::atomic<int> m_encodeWidth{0};
    std::atomic<int> m_encodeHeight{0};
};

}  // namespace codec
}  // namespace TGCapture

#endif  // TGCAPTURE_CODEC_VIDEO_ENCODER_H
