#include "mux/Muxer.h"

#include "core/Clock.h"
#include "core/Logger.h"

extern "C"
{
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

namespace TGCapture
{

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

    m_url = params.muxerOutput;
    m_format = params.muxer.empty() ? "flv" : params.muxer;

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr, m_format.c_str(), m_url.c_str());
    if (ret < 0 || !m_fmt) {
        TG_LOGE("Muxer: 创建输出上下文失败 fmt=%s url=%s", m_format.c_str(), m_url.c_str());
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
            TG_LOGE("Muxer: avio_open 失败 url=%s ret=%d", m_url.c_str(), ret);
            return -2;
        }
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "fflags", "nobuffer", 0);
    ret = avformat_write_header(m_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        TG_LOGE("Muxer: write_header 失败 ret=%d", ret);
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
    m_videoQueue->Push(std::move(packet));
}

void Muxer::SubmitAudioPacket(core::AVPacketPtr packet)
{
    if (!m_opened.load() || !packet) {
        return;
    }
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
        // 音频优先（主时钟），再处理视频，保证交织平稳
        if (auto packet = m_audioQueue->TryPop(std::chrono::milliseconds(10))) {
            WriteOne(std::move(*packet), false);
            continue;
        }
        if (auto packet = m_videoQueue->TryPop(std::chrono::milliseconds(10))) {
            WriteOne(std::move(*packet), true);
        }
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
    av_packet_rescale_ts(packet.get(), AVRational{1, 1000000}, stream->time_base);

    if (m_packets.load() < 3) {
        TG_LOGD("Muxer: 写包 stream=%d pts=%lld size=%d", stream->index,
                static_cast<long long>(packet->pts), packet->size);
    }
    int ret = av_interleaved_write_frame(m_fmt, packet.get());
    if (m_packets.load() < 3) {
        TG_LOGD("Muxer: 写包返回 ret=%d", ret);
    }
    if (ret < 0) {
        TG_LOGW("Muxer: 写包失败 ret=%d (video=%d)", ret, isVideo ? 1 : 0);
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
