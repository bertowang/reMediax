#include "codec/AudioEncoder.h"

#include <cstdio>

#include "core/AvError.h"
#include "core/Logger.h"

extern "C"
{
#include <libavutil/opt.h>
}

namespace TGCapture::codec
{

AudioEncoder::AudioEncoder(size_t queueCapacity)
    : core::BusNode("audio_encoder"),
      core::Worker("audio_encoder"),
      m_queue(std::make_unique<core::BoundedQueue<core::FramePtr>>(queueCapacity, core::DropPolicy::DropOldest))
{
}

AudioEncoder::~AudioEncoder()
{
    Stop();
    Close();
}

int AudioEncoder::Open(const AudioEncoderParams& params, const core::AudioMediaType& srcType)
{
    if (m_opened.load()) {
        return 0;
    }
    // 重开：队列在 Close() 时被永久关闭，必须显式重开
    m_queue->Reopen();
    m_encoded.store(0);
    m_encodeFail.Reset();
    m_encodeFail.Configure(1000000, 200000);
    m_lastAvError.store(0);
    // 上一轮没凑满一帧的残留 PCM 不能带进新一轮，否则开头会多出一段错位音频
    m_pcmBuffer.clear();
    m_nextPts = 0;
    m_srcType = srcType;
    m_codecName = params.aCodec.empty() ? "libfdk_aac" : params.aCodec;

    const AVCodec* codec = avcodec_find_encoder_by_name(m_codecName.c_str());
    if (!codec) {
        TG_LOGW("AudioEncoder: %s 不可用，回退 aac", m_codecName.c_str());
        codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        m_codecName = "aac";
    }
    if (!codec) {
        TG_LOGE("AudioEncoder: 找不到 AAC 编码器");
        m_lastAvError.store(AVERROR_ENCODER_NOT_FOUND);
        return -1;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
        TG_LOGE("AudioEncoder: 分配编码器上下文失败 codec=%s", m_codecName.c_str());
        m_lastAvError.store(AVERROR(ENOMEM));
        return -2;
    }

    const int channels = params.channels > 0 ? params.channels : srcType.Channels();
    m_ctx->sample_rate = params.sampleRate > 0 ? params.sampleRate : srcType.SampleRate();
    // ffmpeg 6.x 使用 ch_layout（channels / channel_layout 已移除）
    av_channel_layout_default(&m_ctx->ch_layout, channels);
    m_ctx->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_S16;
    m_ctx->bit_rate = params.aBitrate > 0 ? params.aBitrate : 128000;
    // 全链路统一微秒时间基：PTS 一律由混流器给的 ptsUs 换算，
    // 不能用"采样数"当 PTS（封装侧按 1/1000000 解释，单位不一致会让文件时间轴错乱）
    m_ctx->time_base = AVRational{1, 1000000};
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    const int openRet = avcodec_open2(m_ctx, codec, nullptr);
    if (openRet < 0) {
        TG_LOGE("AudioEncoder: avcodec_open2 失败 %d (%s) codec=%s", openRet, core::AvErr(openRet),
                m_codecName.c_str());
        m_lastAvError.store(openRet);
        avcodec_free_context(&m_ctx);
        return -3;
    }

    m_frameSize = m_ctx->frame_size > 0 ? m_ctx->frame_size : 1024;
    m_opened.store(true);
    TG_LOGI("AudioEncoder: %s %dHz %dch frame=%d", m_codecName.c_str(), m_ctx->sample_rate,
            m_ctx->ch_layout.nb_channels, m_frameSize);
    return 0;
}

void AudioEncoder::Close()
{
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
    // 残留 PCM 必须在这里放掉：只在 Open() 里 clear 的话，
    // "编码失败 → Stop → 不再重开"这条路径上这块内存会一直挂到对象析构
    m_pcmBuffer.clear();
    m_pcmBuffer.shrink_to_fit();
    m_queue->Close();
    m_opened.store(false);
}

bool AudioEncoder::Submit(core::FramePtr frame)
{
    if (!m_opened.load() || !frame) {
        return false;
    }
    return m_queue->Push(std::move(frame));
}

size_t AudioEncoder::Pending() const
{
    return m_queue ? m_queue->Size() : 0;
}

void AudioEncoder::Loop(std::stop_token token)
{
    while (!token.stop_requested()) {
        auto item = m_queue->TryPop(std::chrono::milliseconds(50));
        if (!item.has_value()) {
            continue;
        }
        EncodeOne(*item);   // 失败的归集与上报在 EncodeOne 内部（OnEncodeResult）
    }
    Drain();
}

void AudioEncoder::OnThreadExit()
{
    TG_LOGI("AudioEncoder: 线程退出 codec=%s encoded=%llu latency_us=%lld",
            m_codecName.c_str(),
            static_cast<unsigned long long>(m_encoded.load()),
            static_cast<long long>(m_encodeLatencyUs.load()));
}

int AudioEncoder::EncodeOne(const core::FramePtr& frame)
{
    if (!m_ctx || !frame || !frame->audioData || frame->audioBytes == 0) {
        return -1;
    }

    const int channels = m_ctx->ch_layout.nb_channels > 0 ? m_ctx->ch_layout.nb_channels : 2;
    const int frameSize = m_frameSize > 0 ? m_frameSize : 1024;

    // 采集侧每次送的样本数不固定，而 AAC 编码器要求固定 frame_size：
    // 这里做累积缓冲，攒够 frameSize 再送一帧（同时避免 AVFrame 缓冲区越界）
    const auto* src = reinterpret_cast<const int16_t*>(frame->audioData.get());
    const size_t samples = frame->audioBytes / sizeof(int16_t);
    m_pcmBuffer.insert(m_pcmBuffer.end(), src, src + samples);

    const size_t need = static_cast<size_t>(frameSize) * static_cast<size_t>(channels);
    // 背压上限：编码持续失败时本函数会提前返回，剩余样本留在缓冲里等下一批。
    // 没有上限的话，缓冲会以采集速率单向增长（44.1kHz 立体声约 176 KB/s）且 Close() 也不清，
    // 最终是一条在故障状态下慢慢吃光内存的路径。超限丢最旧的，保留最新音频。
    const size_t kMaxPending = need * 16;
    if (m_pcmBuffer.size() > kMaxPending) {
        const size_t drop = m_pcmBuffer.size() - kMaxPending;
        TG_LOGW_THROTTLED(1000000, "AudioEncoder: PCM 积压超限，丢弃最旧 %zu 个样本（编码持续失败？）",
                          drop);
        m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + static_cast<long>(drop));
    }
    if (m_pcmBuffer.size() < need) {
        return 0;  // 数据不足，等待下一批
    }

    // 一次提交可能凑出多个编码器帧（采集帧长与编码器 frame_size 不一致时）。
    // 之前每个 AVFrame 都用同一个 ptsUs，会产出多个 PTS 完全相同的音频包；
    // 兜底时钟也错把"样本数"当微秒推进（time_base 是 1/1e6），时间轴被压缩了 20 倍。
    const int64_t frameDurUs =
        (m_ctx->sample_rate > 0)
            ? static_cast<int64_t>(frameSize) * 1000000LL / m_ctx->sample_rate
            : 0;
    const int64_t basePts = frame->ptsUs >= 0
                                ? av_rescale_q(frame->ptsUs, AVRational{1, 1000000}, m_ctx->time_base)
                                : m_nextPts;
    int chunkIndex = 0;

    int ret = 0;
    while (m_pcmBuffer.size() >= need) {
        core::AVFramePtr avFrame = core::MakeAVFrame();
        avFrame->format = m_ctx->sample_fmt;
        avFrame->ch_layout = m_ctx->ch_layout;
        avFrame->sample_rate = m_ctx->sample_rate;
        avFrame->nb_samples = frameSize;   // 先确定样本数，再分配缓冲

        // 取不到缓冲 = 内存不足，按编码失败计入（原先静默 return，宿主完全无感）
        if (int bufRet = av_frame_get_buffer(avFrame.get(), 0); bufRet < 0) {
            TG_LOGE_THROTTLED(1000000, "AudioEncoder: av_frame_get_buffer 失败 %d (%s)", bufRet,
                              core::AvErr(bufRet));
            OnEncodeResult(false, bufRet);
            // 这一块编码不出去了，直接丢掉：留在缓冲里下次还是同样的结果，只是把内存耗尽
            m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + static_cast<long>(need));
            return -2;
        }

        const bool planar = av_sample_fmt_is_planar(m_ctx->sample_fmt) != 0;
        const int16_t* chunk = m_pcmBuffer.data();
        if (planar) {
            for (int c = 0; c < channels; ++c) {
                auto* dst = reinterpret_cast<int16_t*>(avFrame->data[c]);
                for (int i = 0; i < frameSize; ++i) {
                    dst[i] = chunk[static_cast<size_t>(i) * channels + c];
                }
            }
        } else {
            memcpy(avFrame->data[0], chunk, static_cast<size_t>(frameSize) * channels * sizeof(int16_t));
        }
        // 数据已经拷进 AVFrame，这里就消费掉（注意 chunk 随之失效，后面不再用它）。
        // 原先放在循环末尾，send_frame / receive_packet 失败时会跳过它，
        // 于是编码每失败一次缓冲就多留一块，故障期间单向增长。
        m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + static_cast<long>(need));

        avFrame->pts = basePts + frameDurUs * chunkIndex;
        ++chunkIndex;

        ret = avcodec_send_frame(m_ctx, avFrame.get());
        if (ret < 0) {
            TG_LOGE_THROTTLED(1000000, "AudioEncoder: send_frame 失败 %d (%s)", ret, core::AvErr(ret));
            OnEncodeResult(false, ret);
            return -4;
        }
        while (ret >= 0) {
            core::AVPacketPtr packet = core::MakeAVPacket();
            ret = avcodec_receive_packet(m_ctx, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                TG_LOGE_THROTTLED(1000000, "AudioEncoder: receive_packet 失败 %d (%s)", ret, core::AvErr(ret));
                OnEncodeResult(false, ret);
                return -5;
            }
            // 音频主时钟由 TGCapturer 的混流器推进（见 TGCapturer::MixLoop），这里不再重复更新
            DeliverPacket(std::move(packet));
        }

        m_encoded.fetch_add(1);
        OnEncodeResult(true, 0);
    }
    // 兜底时钟按"实际送出的样本数"推进，供下一批没带 ptsUs 的帧使用
    m_nextPts = basePts + frameDurUs * chunkIndex;
    // 端到端编码延迟 EMA（μs）：capture → encode out；captureUs 无效时跳过
    if (frame->captureUs != AV_NOPTS_VALUE) {
        const int64_t latencyUs = core::MonotonicClock::NowUs() - frame->captureUs;
        if (latencyUs >= 0 && latencyUs < 10000000) {
            int64_t cur = m_encodeLatencyUs.load();
            int64_t next = (cur == 0) ? latencyUs : (cur + (latencyUs - cur) / 8);
            m_encodeLatencyUs.store(next);
        }
    }
    return 0;
}

// 与视频编码器同一套判定：连续失败 1s 才上报，期间恢复则只打日志
void AudioEncoder::OnEncodeResult(bool ok, int ret)
{
    const int64_t nowUs = core::MonotonicClock::NowUs();
    if (ok) {
        if (m_encodeFail.OnSuccess(nowUs)) {
            TG_LOGI("AudioEncoder: 编码已恢复 codec=%s", m_codecName.c_str());
        }
        return;
    }
    m_lastAvError.store(ret);
    if (m_encodeFail.OnFailure(nowUs)) {
        char message[160];
        std::snprintf(message, sizeof(message), "audio encode failing for %lldms (%s)",
                      static_cast<long long>(m_encodeFail.FailingUs(nowUs) / 1000),
                      core::AvErr(ret));
        ReportError(TGErr_AudioEncodeFailed, message, ret);
    }
}

int AudioEncoder::Drain()
{
    if (!m_ctx) {
        return 0;
    }
    avcodec_send_frame(m_ctx, nullptr);
    while (true) {
        core::AVPacketPtr packet = core::MakeAVPacket();
        int ret = avcodec_receive_packet(m_ctx, packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
            break;
        }
        DeliverPacket(std::move(packet));
    }
    return 0;
}

bool AudioEncoder::OnCommand(const core::BusCommand& command)
{
    if (command.type == core::BusCommandType::Flush) {
        if (m_queue) {
            m_queue->Clear();
        }
        return true;
    }
    return false;
}

void AudioEncoder::DeliverPacket(core::AVPacketPtr packet)
{
    if (!packet) {
        return;
    }
    auto out = std::make_shared<core::Frame>();
    out->kind = core::MediaKind::Packet;
    out->streamKind = core::MediaKind::Audio;
    out->keyFrame = true;   // 音频包都可独立解码
    out->packet = std::move(packet);
    Forward(std::move(out));
}

}  // namespace TGCapture::codec
