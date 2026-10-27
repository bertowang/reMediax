#include "mux/Muxer.h"

#include "core/AvError.h"
#include "core/Clock.h"
#include "core/Logger.h"
#include "core/UrlRedact.h"

extern "C"
{
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

namespace TGCapture
{

namespace
{
// 还没写出过任何包时的哨兵值
constexpr int64_t kNoPts = INT64_MIN / 4;
}  // namespace

// 音频队列也用 DropOldest：之前是 Block，封装线程卡在网络 I/O 时队列写满，
// 音频编码线程会被反压卡死在 Submit 里 —— 此时 Stop() 先 join 编码线程，而唤醒它的 Close() 排在后面，
// 形成互相等待。宁可丢最旧的音频包，也不能让一个坏掉的下游卡死上游线程。
Muxer::Muxer(size_t queueCapacity)
    : core::BusNode("muxer"),
      core::Worker("muxer"),
      m_videoQueue(std::make_unique<core::BoundedQueue<core::AVPacketPtr>>(queueCapacity, core::DropPolicy::DropOldest)),
      m_audioQueue(std::make_unique<core::BoundedQueue<core::AVPacketPtr>>(queueCapacity, core::DropPolicy::DropOldest))
{
}

Muxer::~Muxer()
{
    Stop();
    Close();
}

int Muxer::Open(const MuxerParams& params, AVCodecContext* videoCtx, AVCodecContext* audioCtx)
{
    if (m_opened.load()) {
        return 0;
    }
    avformat_network_init();

    // 重开：两条队列在 Close() 时被永久关闭，必须显式重开
    m_videoQueue->Reopen();
    m_audioQueue->Reopen();
    m_packets.store(0);
    m_bytes.store(0);
    m_bps.store(0.0f);
    m_lastStatUs = core::MonotonicClock::NowUs();
    m_diagStallVideo.store(0);
    m_diagStallAudio.store(0);
    m_lastVideoArrivalUs.store(0);
    m_lastAudioArrivalUs.store(0);

    m_lastVideoPtsUs.store(kNoPts);
    m_lastAudioPtsUs.store(kNoPts);

    m_abort.store(false);
    m_opStartUs.store(0);
    m_failSinceUs.store(0);
    m_failedPackets.store(0);
    m_failDroppedThisRound = 0;
    m_failReported = false;

    m_url = params.muxerOutput;
    m_urlSafe = core::RedactUrl(m_url);
    m_format = params.muxer.empty() ? "flv" : params.muxer;
    m_connectTimeoutUs = (params.connectTimeoutMs > 0 ? params.connectTimeoutMs : 10000) * 1000LL;
    m_ioTimeoutUs = (params.ioTimeoutMs > 0 ? params.ioTimeoutMs : 5000) * 1000LL;
    m_writeFailReportUs = (params.writeFailReportMs > 0 ? params.writeFailReportMs : 3000) * 1000LL;

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr, m_format.c_str(), m_url.c_str());
    if (ret < 0 || !m_fmt) {
        TG_LOGE("Muxer: 创建输出上下文失败 fmt=%s url=%s ret=%d (%s)", m_format.c_str(),
                m_urlSafe.c_str(), ret, ret < 0 ? core::AvErr(ret) : "ctx=null");
        Close();   // 回滚：否则重试 Prepare 会每失败一次泄漏一个 AVFormatContext
        return -1;
    }

    if (videoCtx) {
        m_videoStream = avformat_new_stream(m_fmt, nullptr);
        if (m_videoStream) {
            avcodec_parameters_from_context(m_videoStream->codecpar, videoCtx);
            m_videoStream->time_base = AVRational{1, 1000000};
            m_videoStream->codecpar->codec_tag = 0;
        }
    }
    if (audioCtx) {
        m_audioStream = avformat_new_stream(m_fmt, nullptr);
        if (m_audioStream) {
            avcodec_parameters_from_context(m_audioStream->codecpar, audioCtx);
            m_audioStream->time_base = AVRational{1, audioCtx->sample_rate};
            m_audioStream->codecpar->codec_tag = 0;
        }
    }

    // 低延迟：每个包写完立刻 flush 到底层 I/O，而不是攒在 avio 缓冲里等下一个包。
    // 注意 flush_packets 属于 AVFormatContext 而不是 flv/mpegts 的私有选项 ——
    // 之前对 priv_data 设置会静默失败（返回值被忽略），低延迟配置实际没有生效。
    m_fmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    m_fmt->flush_packets = 1;

    // 所有可能阻塞的 I/O 都挂中断回调：超时 / Abort 命令都能把它打断
    m_fmt->interrupt_callback.callback = &Muxer::InterruptCb;
    m_fmt->interrupt_callback.opaque = this;

    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        // rw_timeout 是 URLContext 的通用选项，rtmp(tcp) / srt 等协议都认；对文件协议无副作用
        AVDictionary* ioOptions = nullptr;
        av_dict_set_int(&ioOptions, "rw_timeout", m_ioTimeoutUs, 0);
        BeginOp(m_connectTimeoutUs);
        ret = avio_open2(&m_fmt->pb, m_url.c_str(), AVIO_FLAG_WRITE, &m_fmt->interrupt_callback,
                         &ioOptions);
        EndOp();
        av_dict_free(&ioOptions);
        if (ret < 0) {
            TG_LOGE("Muxer: avio_open 失败 url=%s ret=%d (%s)", m_urlSafe.c_str(), ret,
                    core::AvErr(ret));
            Close();
            return -2;
        }
    }

    BeginOp(m_connectTimeoutUs);
    ret = avformat_write_header(m_fmt, nullptr);
    EndOp();
    if (ret < 0) {
        TG_LOGE("Muxer: write_header 失败 ret=%d (%s) url=%s", ret, core::AvErr(ret),
                m_urlSafe.c_str());
        Close();
        return -3;
    }

    m_headerWritten.store(true);
    m_opened.store(true);
    m_lastStatUs = core::MonotonicClock::NowUs();
    TG_LOGI("Muxer: 就绪 fmt=%s url=%s connectTimeout=%lldms ioTimeout=%lldms", m_format.c_str(),
            m_urlSafe.c_str(), static_cast<long long>(m_connectTimeoutUs / 1000),
            static_cast<long long>(m_ioTimeoutUs / 1000));
    return 0;
}

int Muxer::InterruptCb(void* opaque)
{
    auto* self = static_cast<Muxer*>(opaque);
    if (self->m_abort.load(std::memory_order_relaxed)) {
        return 1;
    }
    const int64_t start = self->m_opStartUs.load(std::memory_order_relaxed);
    const int64_t limit = self->m_opLimitUs.load(std::memory_order_relaxed);
    if (start > 0 && limit > 0 && core::MonotonicClock::NowUs() - start > limit) {
        return 1;
    }
    return 0;
}

void Muxer::BeginOp(int64_t limitUs)
{
    m_opLimitUs.store(limitUs);
    m_opStartUs.store(core::MonotonicClock::NowUs());
}

void Muxer::EndOp()
{
    m_opStartUs.store(0);
}

bool Muxer::Submit(core::FramePtr frame)
{
    if (!frame || !frame->packet) {
        return false;
    }
    if (frame->streamKind == core::MediaKind::Audio) {
        SubmitAudioPacket(std::move(frame->packet));
    } else {
        SubmitVideoPacket(std::move(frame->packet));
    }
    return m_opened.load();
}

bool Muxer::OnCommand(const core::BusCommand& command)
{
    switch (command.type) {
        case core::BusCommandType::Flush:
            m_videoQueue->Clear();
            m_audioQueue->Clear();
            return true;
        case core::BusCommandType::Abort:
            // 拆除路径：之后所有 I/O（含 trailer）立即失败返回，不再各等一个超时
            m_abort.store(true);
            TG_LOGW("Muxer: 收到 Abort，打断进行中的 I/O");
            return true;
        default:
            return false;
    }
}

void Muxer::Close()
{
    m_headerWritten.store(false);
    if (m_fmt) {
        if (m_opened.load()) {
            BeginOp(m_ioTimeoutUs);
            av_write_trailer(m_fmt);
            EndOp();
        }
        if (!(m_fmt->oformat->flags & AVFMT_NOFILE) && m_fmt->pb) {
            BeginOp(m_ioTimeoutUs);
            avio_closep(&m_fmt->pb);
            EndOp();
        }
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    // 流指针随 AVFormatContext 一起失效，必须置空：
    // 否则下一轮 Open 之前若被 WriteOne 读到就是野指针。
    m_videoStream = nullptr;
    m_audioStream = nullptr;
    m_videoQueue->Close();
    m_audioQueue->Close();
    m_opened.store(false);
}

void Muxer::SubmitVideoPacket(core::AVPacketPtr packet)
{
    if (!m_opened.load() || !packet) {
        return;
    }
    m_lastVideoArrivalUs.store(core::MonotonicClock::NowUs());
    // 拥塞时优先丢非关键帧：丢掉 IDR 会让后续整个 GOP 都解不出来（花屏），
    // 丢 P 帧最多是短暂无变化，观感与可恢复性都好得多。
    m_videoQueue->PushKeepProtected(
        std::move(packet),
        [](const core::AVPacketPtr& p) { return p && (p->flags & AV_PKT_FLAG_KEY) != 0; });
}

void Muxer::SubmitAudioPacket(core::AVPacketPtr packet)
{
    if (!m_opened.load() || !packet) {
        return;
    }
    m_lastAudioArrivalUs.store(core::MonotonicClock::NowUs());
    m_audioQueue->Push(std::move(packet));
}

size_t Muxer::VideoPending() const
{
    return m_videoQueue ? m_videoQueue->Size() : 0;
}

size_t Muxer::AudioPending() const
{
    return m_audioQueue ? m_audioQueue->Size() : 0;
}

void Muxer::Loop(std::stop_token token)
{
    while (!token.stop_requested()) {
        // 交织控制：哪一路领先太多就先不写它，等落后的一路追上（阈值默认 350ms）
        const int64_t threshold = m_interleaveUs.load();
        const int64_t lastV = m_lastVideoPtsUs.load();
        const int64_t lastA = m_lastAudioPtsUs.load();
        const bool videoTooFar = threshold > 0 && lastV != kNoPts && lastA != kNoPts && (lastV - lastA) > threshold;
        const bool audioTooFar = threshold > 0 && lastV != kNoPts && lastA != kNoPts && (lastA - lastV) > threshold;

        // 某一路不再产出时（设备失效、该路关闭、编码器卡住），不能让另一路永远等下去：
        // 该路"超时未到包"就把它的基准往前推，保证封装继续出包（MediaX 的 BreakA/BreakV 自愈）
        const int64_t nowUs = core::MonotonicClock::NowUs();
        const int64_t stallUs = threshold > 0 ? (threshold * 3) : 0;
        if (threshold > 0 && audioTooFar) {
            const int64_t lastArrival = m_lastAudioArrivalUs.load();
            if (m_audioQueue->Size() == 0 ||
                (lastArrival > 0 && nowUs - lastArrival > stallUs)) {
                m_lastAudioPtsUs.store(m_lastVideoPtsUs.load() - threshold / 2);
                m_diagStallAudio.fetch_add(1);
            }
        }
        if (threshold > 0 && videoTooFar) {
            const int64_t lastArrival = m_lastVideoArrivalUs.load();
            if (m_videoQueue->Size() == 0 ||
                (lastArrival > 0 && nowUs - lastArrival > stallUs)) {
                m_lastVideoPtsUs.store(m_lastAudioPtsUs.load() - threshold / 2);
                m_diagStallVideo.fetch_add(1);
            }
        }

        // 音频是主时钟，正常情况优先写
        if (!videoTooFar) {
            if (auto packet = m_audioQueue->TryPop(std::chrono::milliseconds(10))) {
                WriteOne(std::move(*packet), false);
                continue;
            }
        }
        if (!audioTooFar || m_videoQueue->Size() == 0) {
            if (auto packet = m_videoQueue->TryPop(std::chrono::milliseconds(10))) {
                WriteOne(std::move(*packet), true);
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 退出前尽量把剩余包写出。
    // 一旦写失败就停：下游已经坏了，剩下的包每个都要再等一个 ioTimeout，
    // 队列里最多 128+128 个包，会把 Stop() 拖成几分钟
    bool healthy = true;
    while (healthy) {
        auto packet = m_audioQueue->TryPop(std::chrono::milliseconds(1));
        if (!packet) {
            break;
        }
        healthy = WriteOne(std::move(*packet), false) >= 0;
    }
    while (healthy) {
        auto packet = m_videoQueue->TryPop(std::chrono::milliseconds(1));
        if (!packet) {
            break;
        }
        healthy = WriteOne(std::move(*packet), true) >= 0;
    }
}

int Muxer::WriteOne(core::AVPacketPtr packet, bool isVideo)
{
    if (!m_fmt || !m_opened.load() || !packet) {
        return -1;
    }
    AVStream* stream = isVideo ? m_videoStream : m_audioStream;
    if (!stream) {
        TG_LOGD("Muxer: 流不存在 video=%d", isVideo ? 1 : 0);
        return -2;
    }

    packet->stream_index = stream->index;
    // 交织基准用"统一微秒时间轴"上的 PTS（rescale 之前）
    const int64_t ptsUs = packet->pts;
    av_packet_rescale_ts(packet.get(), AVRational{1, 1000000}, stream->time_base);
    if (ptsUs != AV_NOPTS_VALUE) {
        if (isVideo) {
            m_lastVideoPtsUs.store(ptsUs);
        } else {
            m_lastAudioPtsUs.store(ptsUs);
        }
    }

    if (m_packets.load() < 3) {
        TG_LOGD("Muxer: 写包 stream=%d pts=%lld size=%d", stream->index,
                static_cast<long long>(packet->pts), packet->size);
    }
    const int64_t startUs = core::MonotonicClock::NowUs();
    BeginOp(m_ioTimeoutUs);
    const int packetSize = packet->size;
    int ret = av_interleaved_write_frame(m_fmt, packet.get());
    EndOp();
    if (m_packets.load() < 3) {
        TG_LOGD("Muxer: 写包返回 ret=%d", ret);
    }
    OnWriteResult(ret >= 0, startUs, core::MonotonicClock::NowUs());
    if (ret < 0) {
        // 写包失败 = 数据丢失（文件截断 / 推流断流），是 Error 不是 Warn；
        // 但断流时会每包命中，所以限频，避免刷屏淹没其它日志
        TG_LOGE_THROTTLED(2000000, "Muxer: 写包失败 ret=%d (%s) video=%d pts=%lld", ret,
                          core::AvErr(ret), isVideo ? 1 : 0, static_cast<long long>(ptsUs));
        return -3;
    }
    UpdateBitrate(static_cast<size_t>(packetSize));
    m_packets.fetch_add(1);
    return 0;
}

// 连续失败超过 writeFailReportMs 才上报一次 Error（同一轮失败不重复上报）。
// 注意 FFmpeg 的 AVIOContext 错误是粘性的：网络断开类的失败之后同一连接不会再写成功，
// 所以"恢复"只可能发生在非粘性的瞬时错误上 —— 这里只复位计数，不对外承诺"自动恢复"。
void Muxer::OnWriteResult(bool ok, int64_t startUs, int64_t endUs)
{
    if (ok) {
        if (m_abort.load()) {
            return;   // 中断之后写进 AVIO 缓冲的"成功"不代表链路恢复
        }
        const int64_t since = m_failSinceUs.load();
        if (since != 0) {
            // AVIO 有缓冲：失败后紧接着的几次写只是进了缓冲区，下一次 flush 才会再失败。
            // 要持续成功 1s（缓冲早已刷过几轮）才算真的恢复
            if (m_okSinceUs == 0) {
                m_okSinceUs = endUs;
            }
            if (endUs - m_okSinceUs < 1000000) {
                return;
            }
            if (m_failReported) {
                TG_LOGI("Muxer: 写包已恢复 失败持续=%lldms 期间丢包=%llu",
                        static_cast<long long>((endUs - since) / 1000),
                        static_cast<unsigned long long>(m_failDroppedThisRound));
            }
            m_failSinceUs.store(0);
            m_failDroppedThisRound = 0;
            m_failReported = false;
            m_okSinceUs = 0;
        }
        return;
    }

    m_okSinceUs = 0;
    m_failedPackets.fetch_add(1);
    ++m_failDroppedThisRound;
    if (m_failSinceUs.load() == 0) {
        m_failSinceUs.store(startUs > 0 ? startUs : 1);
    }
    const int64_t durUs = endUs - m_failSinceUs.load();
    if (!m_failReported && durUs >= m_writeFailReportUs) {
        m_failReported = true;
        ReportError(TGErr_MuxWriteFailed, "mux write failing continuously",
                    static_cast<int32_t>(durUs / 1000));
    }
}

void Muxer::UpdateBitrate(size_t bytes)
{
    m_bytes.fetch_add(bytes);
    const int64_t nowUs = core::MonotonicClock::NowUs();
    const int64_t elapsed = nowUs - m_lastStatUs;
    if (elapsed >= 1000000) {
        const uint64_t bytesDelta = m_bytes.load();
        m_bps.store(static_cast<float>(bytesDelta * 8) / (elapsed / 1000000.0f));
        m_bytes.store(0);
        m_lastStatUs = nowUs;
    }
}

float Muxer::TrafficBps() const
{
    return m_bps.load();
}

std::string Muxer::Stats() const
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
             "{\"format\":\"%s\",\"url\":\"%s\",\"bitrate_bps\":%.0f,\"packets\":%llu,"
             "\"video_pending\":%zu,\"audio_pending\":%zu,"
             "\"write_failed\":%llu,\"failing\":%d,"
             "\"video_dropped\":%llu,\"audio_dropped\":%llu}",
             m_format.c_str(), m_urlSafe.c_str(), TrafficBps(),
             static_cast<unsigned long long>(m_packets.load()), VideoPending(), AudioPending(),
             static_cast<unsigned long long>(m_failedPackets.load()), IsFailing() ? 1 : 0,
             static_cast<unsigned long long>(m_videoQueue ? m_videoQueue->DroppedCount() : 0),
             static_cast<unsigned long long>(m_audioQueue ? m_audioQueue->DroppedCount() : 0));
    return std::string(buffer);
}

}  // namespace TGCapture
