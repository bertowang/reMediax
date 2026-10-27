// 封装推流：flv(rtmp) / mpegts(srt) / 本地文件，音视频交织写出
//
// 总线节点（名字 "muxer"）：
//   数据面  Submit(Frame{kind=Packet, streamKind=Video|Audio})；编码器链尾就是它
//   命令面  Flush = 丢弃积压；Abort = 立即打断正在进行的 I/O（拆除路径，不等超时）
//   事件面  写包持续失败超过阈值 → TGErr_MuxWriteFailed（Error）。不自动重连（I/O 错误在 FFmpeg 里是粘性的）
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
#include "core/Queue.h"
#include "core/Thread.h"

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
    [[nodiscard]] bool IsFailing() const noexcept { return m_failSinceUs.load() != 0; }

    // 音视频交织阈值（微秒）：领先的一方暂停写入，等落后的一方追上，0 = 关闭
    void SetInterleaveUs(int64_t us) noexcept { m_interleaveUs.store(us > 0 ? us : 0); }
    [[nodiscard]] int64_t InterleaveUs() const noexcept { return m_interleaveUs.load(); }

    [[nodiscard]] float TrafficBps() const;
    [[nodiscard]] uint64_t WrittenBytes() const noexcept { return m_bytes.load(); }
    [[nodiscard]] uint64_t WrittenPackets() const noexcept { return m_packets.load(); }
    [[nodiscard]] uint64_t FailedPackets() const noexcept { return m_failedPackets.load(); }
    [[nodiscard]] std::string Stats() const;

protected:
    void Loop(std::stop_token token) override;

private:
    int WriteOne(core::AVPacketPtr packet, bool isVideo);
    void UpdateBitrate(size_t bytes);
    void OnWriteResult(bool ok, int64_t startUs, int64_t endUs);

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

    std::atomic<uint64_t> m_bytes{0};
    std::atomic<uint64_t> m_packets{0};
    std::atomic<float> m_bps{0.0f};
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
    int64_t m_lastStatUs = 0;

    // ---- I/O 超时与中断 ----
    std::atomic_bool m_abort{false};
    std::atomic<int64_t> m_opStartUs{0};     // 当前阻塞操作的起点，0 = 不在操作中
    std::atomic<int64_t> m_opLimitUs{0};     // 当前操作允许的最长时间
    int64_t m_connectTimeoutUs = 10000000;
    int64_t m_ioTimeoutUs = 5000000;

    // ---- 写包失败追踪（仅 Muxer 线程写，其余线程只读 m_failSinceUs）----
    int64_t m_writeFailReportUs = 3000000;
    std::atomic<int64_t> m_failSinceUs{0};   // 本轮连续失败的起点，0 = 当前正常
    std::atomic<uint64_t> m_failedPackets{0};
    uint64_t m_failDroppedThisRound = 0;
    bool m_failReported = false;
    int64_t m_okSinceUs = 0;   // 失败后第一次写成功的时刻；持续成功够久才算恢复（缓冲写入的成功不算）
};

}  // namespace TGCapture

#endif  // TGCAPTURE_MUX_MUXER_H
