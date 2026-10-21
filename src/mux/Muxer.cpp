#include "mux/Muxer.h"

#include "core/AvError.h"
#include "core/Clock.h"
#include "core/Logger.h"

extern "C"
{
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

Muxer::Muxer(size_t queueCapacity)
    : core::Worker("muxer"),
      m_videoQueue(std::make_unique<core::BoundedQueue<core::AVPacketPtr>>(queueCapacity, core::DropPolicy::DropOldest)),
      m_audioQueue(std::make_unique<core::BoundedQueue<core::AVPacketPtr>>(queueCapacity, core::DropPolicy::Block))
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

    m_url = params.muxerOutput;
    m_format = params.muxer.empty() ? "flv" : params.muxer;

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr, m_format.c_str(), m_url.c_str());
    if (ret < 0 || !m_fmt) {
        TG_LOGE("Muxer: 创建输出上下文失败 fmt=%s url=%s ret=%d (%s)", m_format.c_str(),
                m_url.c_str(), ret, ret < 0 ? core::AvErr(ret) : "ctx=null");
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

    // 低延迟
    av_opt_set(m_fmt->priv_data, "fflags", "nobuffer", 0);
    av_opt_set(m_fmt->priv_data, "flush_packets", "1", 0);

    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&m_fmt->pb, m_url.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) {
            TG_LOGE("Muxer: avio_open 失败 url=%s ret=%d (%s)", m_url.c_str(), ret,
                    core::AvErr(ret));
            Close();
            return -2;
        }
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "fflags", "nobuffer", 0);
    ret = avformat_write_header(m_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        TG_LOGE("Muxer: write_header 失败 ret=%d (%s) url=%s", ret, core::AvErr(ret), m_url.c_str());
        Close();
        return -3;
    }

    m_headerWritten.store(true);
    m_opened.store(true);
    m_lastStatUs = core::MonotonicClock::NowUs();
    TG_LOGI("Muxer: 就绪 fmt=%s url=%s", m_format.c_str(), m_url.c_str());
    return 0;
}

void Muxer::Close()
{
    m_headerWritten.store(false);
    if (m_fmt) {
        if (m_opened.load()) {
            av_write_trailer(m_fmt);
        }
        if (!(m_fmt->oformat->flags & AVFMT_NOFILE) && m_fmt->pb) {
            avio_closep(&m_fmt->pb);
        }
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
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
    m_videoQueue->Push(std::move(packet));
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
    // 退出前尽量把剩余包写出
    while (auto packet = m_audioQueue->TryPop(std::chrono::milliseconds(1))) {
        WriteOne(std::move(*packet), false);
    }
    while (auto packet = m_videoQueue->TryPop(std::chrono::milliseconds(1))) {
        WriteOne(std::move(*packet), true);
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
    int ret = av_interleaved_write_frame(m_fmt, packet.get());
    if (m_packets.load() < 3) {
        TG_LOGD("Muxer: 写包返回 ret=%d", ret);
    }
    if (ret < 0) {
        // 写包失败 = 数据丢失（文件截断 / 推流断流），是 Error 不是 Warn；
        // 但断流时会每包命中，所以限频，避免刷屏淹没其它日志
        TG_LOGE_THROTTLED(2000000, "Muxer: 写包失败 ret=%d (%s) video=%d pts=%lld", ret,
                          core::AvErr(ret), isVideo ? 1 : 0, static_cast<long long>(ptsUs));
        return -3;
    }
    UpdateBitrate(static_cast<size_t>(packet->size));
    m_packets.fetch_add(1);
    return 0;
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
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "{\"format\":\"%s\",\"url\":\"%s\",\"bitrate_bps\":%.0f,\"packets\":%llu,"
             "\"video_pending\":%zu,\"audio_pending\":%zu}",
             m_format.c_str(), m_url.c_str(), TrafficBps(),
             static_cast<unsigned long long>(m_packets.load()), VideoPending(), AudioPending());
    return std::string(buffer);
}

}  // namespace TGCapture
