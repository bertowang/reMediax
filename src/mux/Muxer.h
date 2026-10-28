// 封装推流：flv(rtmp) / mpegts(srt) / 本地文件，音视频交织写出
//
// 总线节点（名字 "muxer"）：
//   数据面  Submit(Frame{kind=Packet, streamKind=Video|Audio})；编码器链尾就是它
//   命令面  Flush = 丢弃积压；Abort = 立即打断正在进行的 I/O（拆除路径，不等超时）
//   事件面  写包持续失败超过阈值 → TGErr_MuxWriteFailed；写文件尾失败 → TGErr_MuxTrailerFailed。
//           两者的 extra 都是底层 AVERROR。不自动重连（I/O 错误在 FFmpeg 里是粘性的）
//
// 阻塞保护：所有可能阻塞的 I/O（打开 / 写头 / 写包 / 写尾）都挂在 AVIOInterruptCB 上，
// 超过各自的超时就被打断 —— 网络卡死时 Stop() 最坏也只等一个 ioTimeout，而不是永久挂起。
#ifndef TGCAPTURE_MUX_MUXER_H
#define TGCAPTURE_MUX_MUXER_H

#include <atomic>
#include <memory>
#include <string>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Bus.h"
#include "core/Frame.h"
#include "core/Health.h"
#include "core/Queue.h"
#include "core/Thread.h"
#include "core/TrafficMeter.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace TGCapture
{

class Muxer : public core::BusNode, public core::Worker
{
public:
    explicit Muxer(size_t queueCapacity = 128);
    ~Muxer() override;

    int Open(const MuxerParams& params, AVCodecContext* videoCtx, AVCodecContext* audioCtx);
    void Close();

    // ---- BusNode ----
    bool Submit(core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override { return VideoPending() + AudioPending(); }
    bool OnCommand(const core::BusCommand& command) override;

    // 直接提交裸包（单测与 Submit 内部使用）
    void SubmitVideoPacket(core::AVPacketPtr packet);
    void SubmitAudioPacket(core::AVPacketPtr packet);

    [[nodiscard]] size_t VideoPending() const;
    [[nodiscard]] size_t AudioPending() const;
    [[nodiscard]] bool IsOpened() const noexcept { return m_opened.load(); }
    // 写包正处于连续失败中（用于拆除时决定要不要直接 Abort，不等超时）
    [[nodiscard]] bool IsFailing() const noexcept { return m_writeFail.Failing(); }
    // 最后一次底层失败的 AVERROR（0 = 无）。Open 失败后由 TGCapturer 取来填进事件的 extra
    [[nodiscard]] int32_t LastAvError() const noexcept { return m_lastAvError.load(); }

    // 音视频交织阈值（微秒）：领先的一方暂停写入，等落后的一方追上，0 = 关闭
    void SetInterleaveUs(int64_t us) noexcept { m_interleaveUs.store(us > 0 ? us : 0); }
    [[nodiscard]] int64_t InterleaveUs() const noexcept { return m_interleaveUs.load(); }

    // 流量统计：实时（1s 窗口，停滞归 0）+ 累计（自第一包起）两种口径，见 core/TrafficMeter.h。
    //   载荷 = 编码器产出的包大小（可分音视频，和编码码率设置直接可比）；
    //   封装 = 实际写进 AVIO 的字节（含 flv tag / ts 分包等封装开销，≈ 文件大小 / 推流上行量）
    struct TrafficSnapshot
    {
        uint64_t packets = 0;
        uint64_t bps = 0, videoBps = 0, audioBps = 0, ioBps = 0;               // 实时
        uint64_t bytes = 0, videoBytes = 0, audioBytes = 0, ioBytes = 0;       // 累计
        uint64_t avgBps = 0;                                                   // 累计载荷 / 时长
        int64_t durationUs = 0;                                                // 自第一包起
    };
    [[nodiscard]] TrafficSnapshot Traffic() const;
    [[nodiscard]] float TrafficBps() const;   // 实时载荷码率
    [[nodiscard]] uint64_t WrittenBytes() const noexcept { return m_traffic.TotalBytes(); }   // 累计载荷字节
    [[nodiscard]] uint64_t WrittenPackets() const noexcept { return m_packets.load(); }
    [[nodiscard]] uint64_t FailedPackets() const noexcept { return m_writeFail.Failures(); }
    [[nodiscard]] std::string Stats() const;

protected:
    void Loop(std::stop_token token) override;

private:
    int WriteOne(core::AVPacketPtr packet, bool isVideo);
    void UpdateTraffic(size_t payloadBytes, bool isVideo);
    void OnWriteResult(bool ok, int ret, int64_t startUs, int64_t endUs);

    // I/O 中断：返回 1 让 ffmpeg 立即放弃当前阻塞操作
    static int InterruptCb(void* opaque);
    void BeginOp(int64_t limitUs);
    void EndOp();

private:
    AVFormatContext* m_fmt = nullptr;
    AVStream* m_videoStream = nullptr;
    AVStream* m_audioStream = nullptr;
    std::string m_url;       // 仅内部使用（avio_open）；任何日志 / 统计输出必须用 m_urlSafe
    std::string m_urlSafe;   // 已脱敏
    std::string m_format;

    std::unique_ptr<core::BoundedQueue<core::AVPacketPtr>> m_videoQueue;
    std::unique_ptr<core::BoundedQueue<core::AVPacketPtr>> m_audioQueue;

    std::atomic<uint64_t> m_packets{0};
    core::TrafficMeter m_traffic;        // 载荷：音视频合计
    core::TrafficMeter m_videoTraffic;   // 载荷：视频
    core::TrafficMeter m_audioTraffic;   // 载荷：音频
    core::TrafficMeter m_ioTraffic;      // 封装后写进 AVIO 的字节（含文件头 / 尾）
    int64_t m_ioPos = 0;                 // 上次结算时的 avio_tell；仅写包线程（及其停止后的 Close）访问
    std::atomic_bool m_opened{false};
    std::atomic_bool m_headerWritten{false};

    // 交织：记录两路各自最后写出的 PTS（微秒）
    std::atomic<int64_t> m_lastVideoPtsUs{INT64_MIN / 4};
    std::atomic<int64_t> m_lastAudioPtsUs{INT64_MIN / 4};
    std::atomic<int64_t> m_lastVideoArrivalUs{0};
    std::atomic<int64_t> m_lastAudioArrivalUs{0};
    std::atomic<int64_t> m_interleaveUs{350000};
    std::atomic<uint64_t> m_diagStallVideo{0};
    std::atomic<uint64_t> m_diagStallAudio{0};

    // ---- I/O 超时与中断 ----
    std::atomic_bool m_abort{false};
    std::atomic<int64_t> m_opStartUs{0};     // 当前阻塞操作的起点，0 = 不在操作中
    std::atomic<int64_t> m_opLimitUs{0};     // 当前操作允许的最长时间
    int64_t m_connectTimeoutUs = 10000000;
    int64_t m_ioTimeoutUs = 5000000;

    // ---- 失败追踪（判定逻辑见 core/Health.h；只由 Muxer 线程驱动）----
    core::FailureTracker m_writeFail;
    std::atomic<int32_t> m_lastAvError{0};
};

}  // namespace TGCapture

#endif  // TGCAPTURE_MUX_MUXER_H
