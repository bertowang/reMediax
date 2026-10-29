#include "capture/FileAudioSource.h"

#include <algorithm>
#include <cstring>

#include "core/AvError.h"
#include "core/Clock.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace TGCapture::capture
{

namespace
{
constexpr int64_t kChunkUs = 10000;          // 每块 10ms
constexpr int64_t kResyncUs = 200000;        // 落后墙钟超过 200ms（线程被挂起等）就重新对齐，不追赶
constexpr int64_t kMaxSleepUs = 5000;
}  // namespace

FileAudioSource::FileAudioSource(std::string path, bool loop)
    : AudioSourceBase(TypeOfLocalFileAudio), m_path(std::move(path)), m_loop(loop)
{
}

FileAudioSource::~FileAudioSource()
{
    StopCapture();
    CloseFile();
}

int FileAudioSource::Probe(const std::string& path, std::string* error)
{
    auto fail = [&](int code, const std::string& why) {
        if (error) {
            *error = why;
        }
        return code;
    };
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return fail(TGErr_InvalidArgument, std::string("打不开文件：") + core::AvErr(ret));
    }
    int code = TGErr_OK;
    std::string why;
    if ((ret = avformat_find_stream_info(fmt, nullptr)) < 0) {
        code = TGErr_InvalidArgument;
        why = std::string("读取流信息失败：") + core::AvErr(ret);
    } else {
        const AVCodec* codec = nullptr;
        const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (idx == AVERROR_STREAM_NOT_FOUND) {
            code = TGErr_InvalidArgument;
            why = "文件里没有音轨";
        } else if (idx < 0 || !codec) {
            code = TGErr_NotSupported;
            why = "音轨的解码器未编进（当前只支持 AAC）";
        }
    }
    avformat_close_input(&fmt);
    return code == TGErr_OK ? TGErr_OK : fail(code, why);
}

int FileAudioSource::Open(int sampleRate, int channels)
{
    if (sampleRate <= 0 || channels <= 0) {
        return TGErr_InvalidArgument;
    }
    CloseFile();   // Close 之后再 Open、或重复 Open：都从头开始
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_ended.store(false);
    m_loops.store(0);
    return OpenFile();
}

void FileAudioSource::Close()
{
    CloseFile();
}

int FileAudioSource::OpenFile()
{
    int ret = avformat_open_input(&m_fmt, m_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        TG_LOGE("FileAudioSource: 打不开 %s：%s", m_path.c_str(), core::AvErr(ret));
        m_fmt = nullptr;
        return TGErr_InvalidArgument;
    }
    if ((ret = avformat_find_stream_info(m_fmt, nullptr)) < 0) {
        TG_LOGE("FileAudioSource: 读取流信息失败 %s：%s", m_path.c_str(), core::AvErr(ret));
        CloseFile();
        return TGErr_InvalidArgument;
    }
    const AVCodec* codec = nullptr;
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (m_stream < 0 || !codec) {
        TG_LOGE("FileAudioSource: %s 没有可解码的音轨（ret=%d）", m_path.c_str(), m_stream);
        CloseFile();
        return TGErr_NotSupported;
    }
    AVStream* st = m_fmt->streams[m_stream];
    // 只读音轨：其它流的包直接在解封装层丢弃
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        m_fmt->streams[i]->discard = static_cast<int>(i) == m_stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    m_dec = avcodec_alloc_context3(codec);
    if (!m_dec || avcodec_parameters_to_context(m_dec, st->codecpar) < 0 ||
        (ret = avcodec_open2(m_dec, codec, nullptr)) < 0) {
        TG_LOGE("FileAudioSource: 解码器 %s 打开失败：%s", codec->name, core::AvErr(ret));
        CloseFile();
        return TGErr_NotSupported;
    }
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_pkt || !m_frame) {
        CloseFile();
        return TGErr_Internal;
    }
    m_decoderDrained = false;
    m_pcm.clear();
    if (st->duration > 0) {
        m_durationUs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000000});
    } else if (m_fmt->duration > 0) {
        m_durationUs = m_fmt->duration;
    } else {
        m_durationUs = 0;
    }
    TG_LOGI("FileAudioSource: 打开 %s（%s %dHz %dch 时长 %.2fs）→ 输出 %dHz %dch loop=%d", m_path.c_str(),
            codec->name, m_dec->sample_rate, m_dec->ch_layout.nb_channels, m_durationUs / 1e6, m_sampleRate,
            m_channels, m_loop ? 1 : 0);
    return TGErr_OK;
}

void FileAudioSource::CloseFile()
{
    swr_free(&m_swr);
    av_frame_free(&m_frame);
    av_packet_free(&m_pkt);
    avcodec_free_context(&m_dec);
    if (m_fmt) {
        avformat_close_input(&m_fmt);
    }
    m_stream = -1;
    m_pcm.clear();
}

bool FileAudioSource::Rewind()
{
    const AVStream* st = m_fmt->streams[m_stream];
    const int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const int ret = av_seek_frame(m_fmt, m_stream, start, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        TG_LOGW("FileAudioSource: 回到开头失败：%s", core::AvErr(ret));
        return false;
    }
    avcodec_flush_buffers(m_dec);
    m_decoderDrained = false;
    return true;
}

bool FileAudioSource::Fill(size_t samples)
{
    const size_t want = samples * static_cast<size_t>(m_channels);
    while (m_pcm.size() < want) {
        int ret = avcodec_receive_frame(m_dec, m_frame);
        if (ret == 0) {
            if (!m_swr) {
                AVChannelLayout outLayout{};
                av_channel_layout_default(&outLayout, m_channels);
                ret = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_S16, m_sampleRate, &m_frame->ch_layout,
                                          static_cast<AVSampleFormat>(m_frame->format), m_frame->sample_rate, 0,
                                          nullptr);
                av_channel_layout_uninit(&outLayout);
                if (ret < 0 || swr_init(m_swr) < 0) {
                    TG_LOGE("FileAudioSource: 重采样器初始化失败");
                    swr_free(&m_swr);
                    av_frame_unref(m_frame);
                    return false;
                }
            }
            const int outCap = swr_get_out_samples(m_swr, m_frame->nb_samples);
            if (outCap > 0) {
                const size_t old = m_pcm.size();
                m_pcm.resize(old + static_cast<size_t>(outCap) * m_channels);
                uint8_t* out = reinterpret_cast<uint8_t*>(m_pcm.data() + old);
                const int got = swr_convert(m_swr, &out, outCap, const_cast<const uint8_t**>(m_frame->extended_data),
                                            m_frame->nb_samples);
                m_pcm.resize(old + static_cast<size_t>(std::max(got, 0)) * m_channels);
            }
            av_frame_unref(m_frame);
            continue;
        }
        if (ret == AVERROR_EOF) {
            return false;   // 解码器已吐完
        }
        if (ret != AVERROR(EAGAIN)) {
            TG_LOGW("FileAudioSource: 解码失败：%s", core::AvErr(ret));
            return false;
        }
        if (m_decoderDrained) {
            return false;
        }
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            // 文件读完：冲刷解码器里剩下的帧
            avcodec_send_packet(m_dec, nullptr);
            m_decoderDrained = true;
            continue;
        }
        if (m_pkt->stream_index == m_stream) {
            if ((ret = avcodec_send_packet(m_dec, m_pkt)) < 0 && ret != AVERROR(EAGAIN)) {
                TG_LOGW("FileAudioSource: 送包失败：%s（跳过该包）", core::AvErr(ret));
            }
        }
        av_packet_unref(m_pkt);
    }
    return true;
}

void FileAudioSource::Loop(std::stop_token st)
{
    if (!m_fmt) {
        // Open 失败：该路不出帧（节点已打过"按静音处理"的日志），线程等到停止
        while (!st.stop_requested()) {
            SleepFor(std::chrono::milliseconds(10));
        }
        return;
    }
    const int chunk = static_cast<int>(m_sampleRate * kChunkUs / 1000000);
    const size_t chunkValues = static_cast<size_t>(chunk) * m_channels;
    // 计划时刻按已送出的样本数推算（而不是块数 × 10ms）：采样率不能被 100 整除时也不累积误差
    int64_t t0 = core::MonotonicClock::NowUs();
    int64_t sent = 0;
    auto dueOf = [&] { return t0 + sent * 1000000 / m_sampleRate; };
    while (!st.stop_requested()) {
        int64_t due = dueOf();
        const int64_t now = core::MonotonicClock::NowUs();
        if (now < due) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::min(due - now, kMaxSleepUs)));
            continue;
        }
        if (now - due > kResyncUs) {
            TG_LOGW("FileAudioSource: 落后 %lldms，重新对齐墙钟", static_cast<long long>((now - due) / 1000));
            t0 = now - sent * 1000000 / m_sampleRate;
            due = dueOf();
        }
        if (m_ended.load()) {
            // 播完后继续按节拍出静音：该路"出过声"，断流会让混音器停止出帧并触发断流看护
            // （文件是唯一音频源时整条音频就断了）；静音帧让时间轴照常推进，宿主收到 EOF 后自行决定是否移除
            m_pcm.assign(chunkValues, 0);
        } else if (m_pcm.size() < chunkValues && !Fill(static_cast<size_t>(chunk))) {
            bool more = false;
            if (m_loop) {
                // 无缝循环：不冲刷重采样器（尾部几个样本留在里面，接着下一遍的开头输出），直接回到开头继续解码。
                // 一整遍都没解出样本（空音轨）就当作结束，别死循环
                const size_t before = m_pcm.size();
                if (Rewind() && Fill(static_cast<size_t>(chunk))) {
                    more = true;
                } else {
                    more = m_pcm.size() > before;
                }
                if (more) {
                    m_loops.fetch_add(1);
                    TG_LOGI("FileAudioSource: %s 第 %u 遍播完，从头循环", m_path.c_str(), m_loops.load());
                }
            }
            if (!more) {
                // 冲刷重采样器的尾巴
                if (m_swr) {
                    const int tail = swr_get_out_samples(m_swr, 0);
                    if (tail > 0) {
                        const size_t old = m_pcm.size();
                        m_pcm.resize(old + static_cast<size_t>(tail) * m_channels);
                        uint8_t* out = reinterpret_cast<uint8_t*>(m_pcm.data() + old);
                        const int got = swr_convert(m_swr, &out, tail, nullptr, 0);
                        m_pcm.resize(old + static_cast<size_t>(std::max(got, 0)) * m_channels);
                    }
                }
                // 不足一块的尾巴补静音，凑成整块送出
                m_pcm.resize(chunkValues, 0);
                m_ended.store(true);
                TG_LOGI("FileAudioSource: %s 播放结束（之后输出静音）", m_path.c_str());
                if (m_onEof) {
                    m_onEof();
                }
            }
        }
        const size_t values = std::min(chunkValues, m_pcm.size());
        const size_t bytes = values * sizeof(int16_t);
        auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
        if (m_muted.load()) {
            std::memset(buffer.get(), 0, bytes);
        } else {
            std::memcpy(buffer.get(), m_pcm.data(), bytes);
        }
        m_pcm.erase(m_pcm.begin(), m_pcm.begin() + static_cast<std::ptrdiff_t>(values));

        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Audio;
        frame->sampleRate = m_sampleRate;
        frame->channels = m_channels;
        frame->sampleFmt = AV_SAMPLE_FMT_S16;
        frame->planar = false;
        frame->nbSamples = static_cast<int>(values / m_channels);
        frame->audioData = buffer;
        frame->audioBytes = bytes;
        // 时间戳 = 本块的计划时刻：严格按样本时长推进，混音器测出的"漂移"为 0
        frame->captureUs = due;
        frame->ptsUs = due;
        sent += frame->nbSamples;
        Emit(std::move(frame));
    }
}

}  // namespace TGCapture::capture
