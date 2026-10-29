#include "mux/Muxer.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

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
    // 全局网络初始化是**带引用计数**的，而 Muxer 这边没有配对的 deinit
    // （在 Close 里 deinit 会把宿主其它并存的 muxer 一起拆掉）。
    // 原先每次 Open（含失败重试）都 +1 而永不 -1，计数单向增长。
    // 这里收敛成进程级一次：正常路径由 ITGCapturerMgr::Init/UnInit 配对，
    // 这一次只是兜住"宿主没调 Init 就直接推流"的用法，总量有界。
    static std::once_flag s_networkOnce;
    std::call_once(s_networkOnce, [] { avformat_network_init(); });

    // 重开：两条队列在 Close() 时被永久关闭，必须显式重开
    m_videoQueue->Reopen();
    m_audioQueue->Reopen();
    m_packets.store(0);
    m_traffic.Reset();
    m_videoTraffic.Reset();
    m_audioTraffic.Reset();
    m_ioTraffic.Reset();
    m_ioPos = 0;   // 文件头在 write_header 里写出，随第一包一起结算进封装字节
    m_diagStallVideo.store(0);
    m_diagStallAudio.store(0);
    m_lastVideoArrivalUs.store(0);
    m_lastAudioArrivalUs.store(0);

    m_lastVideoPtsUs.store(kNoPts);
    m_lastAudioPtsUs.store(kNoPts);

    m_abort.store(false);
    m_opStartUs.store(0);
    m_writeFail.Reset();
    m_lastAvError.store(0);

    m_url = params.muxerOutput;
    m_urlSafe = core::RedactUrl(m_url);
    m_format = params.muxer.empty() ? "flv" : params.muxer;
    m_connectTimeoutUs = (params.connectTimeoutMs > 0 ? params.connectTimeoutMs : 10000) * 1000LL;
    m_ioTimeoutUs = (params.ioTimeoutMs > 0 ? params.ioTimeoutMs : 5000) * 1000LL;
    // 连续失败多久算故障；恢复观察期固定 1s —— AVIO 带缓冲，断流后紧接着的几次写
    // 只是进了缓冲区，必须等缓冲刷过几轮仍然成功才算真的恢复
    m_writeFail.Configure((params.writeFailReportMs > 0 ? params.writeFailReportMs : 3000) * 1000LL,
                          1000000);

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr, m_format.c_str(), m_url.c_str());
    if (ret < 0 || !m_fmt) {
        TG_LOGE("Muxer: 创建输出上下文失败 fmt=%s url=%s ret=%d (%s)", m_format.c_str(),
                m_urlSafe.c_str(), ret, ret < 0 ? core::AvErr(ret) : "ctx=null");
        m_lastAvError.store(ret < 0 ? ret : AVERROR(EINVAL));
        Close();   // 回滚：否则重试 Prepare 会每失败一次泄漏一个 AVFormatContext
        return -1;
    }

    // 建流失败必须让 Open 失败：原先只是跳过，结果是"流打开成功、每个包都被静默丢弃"，
    // 表现为能连上但没有画面/声音，且全链路没有任何错误
    if (videoCtx) {
        m_videoStream = avformat_new_stream(m_fmt, nullptr);
        if (!m_videoStream) {
            TG_LOGE("Muxer: 创建视频流失败 fmt=%s", m_format.c_str());
            m_lastAvError.store(AVERROR(ENOMEM));
            Close();
            return -4;
        }
        ret = avcodec_parameters_from_context(m_videoStream->codecpar, videoCtx);
        if (ret < 0) {
            TG_LOGE("Muxer: 拷贝视频流参数失败 ret=%d (%s)", ret, core::AvErr(ret));
            m_lastAvError.store(ret);
            Close();
            return -4;
        }
        m_videoStream->time_base = AVRational{1, 1000000};
        m_videoStream->codecpar->codec_tag = 0;
    }
    if (audioCtx) {
        m_audioStream = avformat_new_stream(m_fmt, nullptr);
        if (!m_audioStream) {
            TG_LOGE("Muxer: 创建音频流失败 fmt=%s", m_format.c_str());
            m_lastAvError.store(AVERROR(ENOMEM));
            Close();
            return -5;
        }
        ret = avcodec_parameters_from_context(m_audioStream->codecpar, audioCtx);
        if (ret < 0) {
            TG_LOGE("Muxer: 拷贝音频流参数失败 ret=%d (%s)", ret, core::AvErr(ret));
            m_lastAvError.store(ret);
            Close();
            return -5;
        }
        m_audioStream->time_base = AVRational{1, audioCtx->sample_rate};
        m_audioStream->codecpar->codec_tag = 0;
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
            m_lastAvError.store(ret);
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
        m_lastAvError.store(ret);
        Close();
        return -3;
    }

    m_headerWritten.store(true);
    m_opened.store(true);
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
    // 先落下"已打开"标志，再碰 m_fmt。WriteOne 的守卫是 `!m_fmt || !m_opened`，
    // 原先顺序相反（先 free 再清标志），写包线程存在一个窗口能通过守卫拿到已释放的上下文。
    // 真正的保证仍然来自 StopLocked 先 join 写包线程再 Close()，这里只是不再自相矛盾。
    const bool wasOpened = m_opened.exchange(false);
    if (m_fmt) {
        // 写尾失败 = 产出文件缺索引 / 时长不对（flv 的 duration、mp4 的 moov），
        // 对宿主是"文件不可播"级别的问题，不能像以前那样丢掉返回值。
        // Abort 之后的失败是拆除路径的预期结果（我们自己打断的），不上报。
        int ret = 0;
        if (wasOpened) {
            BeginOp(m_ioTimeoutUs);
            ret = av_write_trailer(m_fmt);
            EndOp();
            if (ret < 0 && !m_abort.load()) {
                TG_LOGE("Muxer: 写文件尾失败 ret=%d (%s) url=%s", ret, core::AvErr(ret),
                        m_urlSafe.c_str());
                m_lastAvError.store(ret);
                ReportError(TGErr_MuxTrailerFailed, "write trailer failed", ret);
            } else if (ret < 0) {
                TG_LOGW("Muxer: 写文件尾被中断（拆除路径）ret=%d", ret);
            }
            // 文件尾也计入封装字节，累计值才能和最终文件大小对上（写包线程此时已停止）。
            // 不能只看 avio_tell：flv 写尾会 seek 回文件头回填 duration/filesize，之后位置停在头部附近，
            // 而写尾期间 lavf 交织队列里的残包 + EOS tag 已经写到了文件末尾。
            // 可 seek 的输出（本地文件）取真实大小；推流不可 seek，写尾不会回跳，tell 即末尾
            if (m_fmt->pb) {
                avio_flush(m_fmt->pb);
                int64_t end = (m_fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) ? avio_size(m_fmt->pb) : -1;
                if (end < 0) {
                    end = avio_tell(m_fmt->pb);
                }
                if (end > m_ioPos) {
                    m_ioTraffic.Add(static_cast<uint64_t>(end - m_ioPos), core::MonotonicClock::NowUs());
                    m_ioPos = end;
                }
            }
        }
        if (!(m_fmt->oformat->flags & AVFMT_NOFILE) && m_fmt->pb) {
            BeginOp(m_ioTimeoutUs);
            ret = avio_closep(&m_fmt->pb);
            EndOp();
            if (ret < 0) {
                // 关闭失败只影响已写出的数据能否落盘，单独记日志；错误码已由上面的写尾覆盖
                TG_LOGW("Muxer: 关闭输出失败 ret=%d (%s)", ret, core::AvErr(ret));
            }
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

// 编码器运行期重开（切分辨率）后，第一个关键包带着新的 SPS/PPS（AV_PKT_DATA_NEW_EXTRADATA）。
//   flv   ：flvenc 自己认这个 side data —— 与 codecpar 比较后更新并重发 AVC sequence header。
//           这里不能先替它改 codecpar，否则它比较时发现相同，就不会重发了。
//   其它  ：mpegts 在每个 IDR 前插入的参数集取自 codecpar->extradata，必须由我们替换，
//           否则新分辨率的 IDR 前面挂的是旧 SPS，播放端按旧尺寸解码 → 花屏。
void Muxer::ApplyNewExtradata(AVStream* stream, const AVPacket* packet)
{
    size_t size = 0;
    const uint8_t* side = av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, &size);
    if (!side || size == 0 || !stream || !stream->codecpar) {
        return;
    }
    AVCodecParameters* par = stream->codecpar;
    if (m_format == "flv") {
        TG_LOGI("Muxer: 视频参数集更新（flv 重发 sequence header）size=%zu", size);
        return;
    }
    if (static_cast<size_t>(par->extradata_size) == size && par->extradata &&
        memcmp(par->extradata, side, size) == 0) {
        return;
    }
    auto* buf = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!buf) {
        TG_LOGE("Muxer: 更新视频参数集失败（内存不足）");
        return;
    }
    memcpy(buf, side, size);
    av_freep(&par->extradata);
    par->extradata = buf;
    par->extradata_size = static_cast<int>(size);
    TG_LOGI("Muxer: 视频参数集更新 fmt=%s size=%zu", m_format.c_str(), size);
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
    if (isVideo) {
        ApplyNewExtradata(stream, packet.get());
    }
    const int64_t startUs = core::MonotonicClock::NowUs();
    BeginOp(m_ioTimeoutUs);
    const int packetSize = packet->size;
    int ret = av_interleaved_write_frame(m_fmt, packet.get());
    EndOp();
    if (m_packets.load() < 3) {
        TG_LOGD("Muxer: 写包返回 ret=%d", ret);
    }
    OnWriteResult(ret >= 0, ret, startUs, core::MonotonicClock::NowUs());
    if (ret < 0) {
        // 写包失败 = 数据丢失（文件截断 / 推流断流），是 Error 不是 Warn；
        // 但断流时会每包命中，所以限频，避免刷屏淹没其它日志
        TG_LOGE_THROTTLED(2000000, "Muxer: 写包失败 ret=%d (%s) video=%d pts=%lld", ret,
                          core::AvErr(ret), isVideo ? 1 : 0, static_cast<long long>(ptsUs));
        return -3;
    }
    UpdateTraffic(static_cast<size_t>(packetSize), isVideo);
    m_packets.fetch_add(1);
    return 0;
}

// 写包结果归集到统一的失败追踪器（core/Health.h）：连续失败超过 writeFailReportMs 上报一次
// TGErr_MuxWriteFailed，extra = 底层 AVERROR（与全局 extra 约定一致，持续时长写在 message 里）。
//
// 注意 FFmpeg 的 AVIOContext 错误是粘性的：网络断开类的失败之后同一连接不会再写成功，
// 所以"恢复"只可能发生在非粘性的瞬时错误（短暂磁盘忙等）上 —— 不对外承诺自动恢复。
void Muxer::OnWriteResult(bool ok, int ret, int64_t startUs, int64_t endUs)
{
    if (ok) {
        if (m_abort.load()) {
            return;   // 中断之后写进 AVIO 缓冲的"成功"不代表链路恢复
        }
        const int64_t failingUs = m_writeFail.FailingUs(endUs);
        const uint64_t lost = m_writeFail.RoundFailures();
        if (m_writeFail.OnSuccess(endUs)) {
            TG_LOGI("Muxer: 写包已恢复 失败持续=%lldms 期间丢包=%llu",
                    static_cast<long long>(failingUs / 1000),
                    static_cast<unsigned long long>(lost));
        }
        return;
    }

    m_lastAvError.store(ret);
    const int64_t at = startUs > 0 ? startUs : endUs;
    if (m_writeFail.OnFailure(at)) {
        char message[160];
        std::snprintf(message, sizeof(message),
                      "mux write failing for %lldms, %llu packets lost (%s)",
                      static_cast<long long>(m_writeFail.FailingUs(endUs) / 1000),
                      static_cast<unsigned long long>(m_writeFail.RoundFailures()),
                      core::AvErr(ret));
        ReportError(TGErr_MuxWriteFailed, message, ret);
    }
}

void Muxer::UpdateTraffic(size_t payloadBytes, bool isVideo)
{
    const int64_t nowUs = core::MonotonicClock::NowUs();
    m_traffic.Add(payloadBytes, nowUs);
    (isVideo ? m_videoTraffic : m_audioTraffic).Add(payloadBytes, nowUs);
    // 封装字节取 AVIO 位置增量：flush_packets=1 下每包写完即刷出，增量就是本包实际写出的量
    // （flv tag 头、ts 188 字节分包 / PAT·PMT·PCR 等开销都在里面）
    if (m_fmt && m_fmt->pb) {
        const int64_t pos = avio_tell(m_fmt->pb);
        if (pos > m_ioPos) {
            m_ioTraffic.Add(static_cast<uint64_t>(pos - m_ioPos), nowUs);
            m_ioPos = pos;
        }
    }
}

float Muxer::TrafficBps() const
{
    return static_cast<float>(m_traffic.Bps(core::MonotonicClock::NowUs()));
}

Muxer::TrafficSnapshot Muxer::Traffic() const
{
    const int64_t nowUs = core::MonotonicClock::NowUs();
    TrafficSnapshot s;
    s.packets = m_packets.load();
    s.bps = m_traffic.Bps(nowUs);
    s.videoBps = m_videoTraffic.Bps(nowUs);
    s.audioBps = m_audioTraffic.Bps(nowUs);
    s.ioBps = m_ioTraffic.Bps(nowUs);
    s.bytes = m_traffic.TotalBytes();
    s.videoBytes = m_videoTraffic.TotalBytes();
    s.audioBytes = m_audioTraffic.TotalBytes();
    s.ioBytes = m_ioTraffic.TotalBytes();
    s.avgBps = m_traffic.AverageBps(nowUs);
    s.durationUs = m_traffic.ElapsedUs(nowUs);
    return s;
}

std::string Muxer::Stats() const
{
    // url 长度不定（带鉴权参数的推流地址可达数百字节）：按实际长度留余量，截断会产出半个 JSON
    std::vector<char> buffer(512 + m_format.size() + m_urlSafe.size());
    snprintf(buffer.data(), buffer.size(),
             "{\"format\":\"%s\",\"url\":\"%s\",\"bitrate_bps\":%.0f,\"packets\":%llu,"
             "\"video_pending\":%zu,\"audio_pending\":%zu,"
             "\"write_failed\":%llu,\"failing\":%d,\"last_averror\":%d,"
             "\"video_dropped\":%llu,\"audio_dropped\":%llu}",
             m_format.c_str(), m_urlSafe.c_str(), TrafficBps(),
             static_cast<unsigned long long>(m_packets.load()), VideoPending(), AudioPending(),
             static_cast<unsigned long long>(FailedPackets()), IsFailing() ? 1 : 0,
             m_lastAvError.load(),
             static_cast<unsigned long long>(m_videoQueue ? m_videoQueue->DroppedCount() : 0),
             static_cast<unsigned long long>(m_audioQueue ? m_audioQueue->DroppedCount() : 0));
    return std::string(buffer.data());
}

}  // namespace TGCapture
