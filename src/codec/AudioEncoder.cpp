#include "codec/AudioEncoder.h"

#include "core/AvError.h"
#include "core/Logger.h"
#include "mux/Muxer.h"

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
        return -1;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
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
        if (EncodeOne(*item) < 0) {
            TG_LOGW("AudioEncoder: 编码一帧失败");
        }
    }
    Drain();
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

        if (av_frame_get_buffer(avFrame.get(), 0) < 0) {
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

        avFrame->pts = basePts + frameDurUs * chunkIndex;
        ++chunkIndex;

        ret = avcodec_send_frame(m_ctx, avFrame.get());
        if (ret < 0) {
            TG_LOGE_THROTTLED(1000000, "AudioEncoder: send_frame 失败 %d (%s)", ret, core::AvErr(ret));
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
                return -5;
            }
            // 音频主时钟由 TGCapturer 的混流器推进（见 TGCapturer::MixLoop），这里不再重复更新
            DeliverPacket(std::move(packet));
        }

        m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + static_cast<long>(need));
        m_encoded.fetch_add(1);
    }
    // 兜底时钟按"实际送出的样本数"推进，供下一批没带 ptsUs 的帧使用
    m_nextPts = basePts + frameDurUs * chunkIndex;
    return 0;
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

void AudioEncoder::DeliverPacket(core::AVPacketPtr packet)
{
    if (m_muxer && packet) {
        m_muxer->SubmitAudioPacket(std::move(packet));
    }
}

}  // namespace TGCapture::codec
