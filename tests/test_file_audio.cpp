// 文件音频源（AudioSourceKind::File）：
//   FileAudioSource 单元：按实时节奏出帧、内容就是文件音轨（频率对得上）、循环无缝、播完 EOF 后出静音；
//   门面：参数 / 文件校验同步报错；推流中加文件源，输出音频含文件内容且时间轴连续，EOF 通知
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TGCapturer.h"
#include "capture/FileAudioSource.h"
#include "core/Clock.h"
#include "tg_test.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

using namespace TGCapture;
using namespace std::chrono_literals;

namespace
{

constexpr int kRate = 48000;
constexpr int kCh = 2;

std::string TmpPath(const char* name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

// 用 ffmpeg 自带 aac 编码器写一个纯音频 flv：freq Hz 正弦，durSec 秒，sampleRate / 双声道
bool WriteSineAac(const std::string& path, double freq, double durSec, int sampleRate)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec) return false;
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "flv", path.c_str()) < 0) return false;
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    enc->sample_rate = sampleRate;
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&enc->ch_layout, 2);
    enc->bit_rate = 128000;
    enc->time_base = AVRational{1, sampleRate};
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    bool ok = avcodec_open2(enc, codec, nullptr) >= 0;
    AVStream* st = ok ? avformat_new_stream(fmt, nullptr) : nullptr;
    ok = ok && st && avcodec_parameters_from_context(st->codecpar, enc) >= 0;
    if (ok) st->time_base = enc->time_base;
    ok = ok && avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(fmt, nullptr) >= 0;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const int64_t total = static_cast<int64_t>(durSec * sampleRate);
    int64_t pos = 0;
    auto drain = [&] {
        while (avcodec_receive_packet(enc, pkt) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
        }
    };
    while (ok && pos < total) {
        frame->nb_samples = enc->frame_size;
        frame->format = enc->sample_fmt;
        av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
        frame->sample_rate = sampleRate;
        ok = av_frame_get_buffer(frame, 0) >= 0;
        for (int i = 0; ok && i < frame->nb_samples; ++i) {
            const float v = pos + i < total ? 0.4f * static_cast<float>(std::sin(2 * M_PI * freq * (pos + i) / sampleRate))
                                            : 0.0f;
            reinterpret_cast<float*>(frame->data[0])[i] = v;
            reinterpret_cast<float*>(frame->data[1])[i] = v;
        }
        frame->pts = pos;
        pos += frame->nb_samples;
        ok = ok && avcodec_send_frame(enc, frame) >= 0;
        av_frame_unref(frame);
        drain();
    }
    if (ok) {
        avcodec_send_frame(enc, nullptr);
        drain();
        av_write_trailer(fmt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    if (fmt->pb) avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return ok;
}

// 交错 S16 的主频估计：过零次数（上升沿）/ 时长
double ZeroCrossFreq(const std::vector<int16_t>& pcm, int channels, int rate, size_t begin, size_t end)
{
    int crossings = 0;
    for (size_t i = begin + 1; i < end; ++i) {
        const int16_t a = pcm[(i - 1) * channels];
        const int16_t b = pcm[i * channels];
        crossings += (a < 0 && b >= 0);
    }
    return end > begin ? crossings * static_cast<double>(rate) / static_cast<double>(end - begin) : 0.0;
}

double Rms(const std::vector<int16_t>& pcm, int channels, size_t begin, size_t end)
{
    double acc = 0;
    for (size_t i = begin; i < end; ++i) acc += static_cast<double>(pcm[i * channels]) * pcm[i * channels];
    return end > begin ? std::sqrt(acc / static_cast<double>(end - begin)) : 0.0;
}

// FileAudioSource 直连：收集 durMs 内的全部输出
struct Collector
{
    std::mutex mutex;
    std::vector<int16_t> pcm;
    std::vector<int64_t> pts;
    std::vector<int> sizes;
    std::atomic<int> eofs{0};

    void Attach(capture::FileAudioSource& src)
    {
        src.SetEmitCallback([this](core::FramePtr f) {
            std::lock_guard<std::mutex> lock(mutex);
            const auto* p = reinterpret_cast<const int16_t*>(f->audioData.get());
            pcm.insert(pcm.end(), p, p + f->audioBytes / sizeof(int16_t));
            pts.push_back(f->ptsUs);
            sizes.push_back(f->nbSamples);
        });
        src.SetEofCallback([this] { eofs.fetch_add(1); });
    }
};

// 解码输出文件的音轨为 S16 交错（不重采样：编码器配置就是 kRate / kCh）
struct DecodedAudio
{
    std::vector<int16_t> pcm;
    int rate = 0;
    double maxGapMs = 0;
};

DecodedAudio DecodeAudio(const std::string& path)
{
    DecodedAudio out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    avformat_find_stream_info(fmt, nullptr);
    const AVCodec* codec = nullptr;
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (idx < 0 || !codec) {
        avformat_close_input(&fmt);
        return out;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
    avcodec_open2(dec, codec, nullptr);
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t last = AV_NOPTS_VALUE, lastDur = 0;
    const AVRational tb = fmt->streams[idx]->time_base;
    auto drain = [&] {
        while (avcodec_receive_frame(dec, frame) >= 0) {
            if (!swr) {
                AVChannelLayout outLayout{};
                av_channel_layout_default(&outLayout, kCh);
                swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, frame->sample_rate, &frame->ch_layout,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                swr_init(swr);
                out.rate = frame->sample_rate;
            }
            const size_t old = out.pcm.size();
            out.pcm.resize(old + static_cast<size_t>(frame->nb_samples) * kCh);
            uint8_t* dst = reinterpret_cast<uint8_t*>(out.pcm.data() + old);
            const int got = swr_convert(swr, &dst, frame->nb_samples, const_cast<const uint8_t**>(frame->extended_data),
                                        frame->nb_samples);
            out.pcm.resize(old + static_cast<size_t>(std::max(got, 0)) * kCh);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx) {
            if (last != AV_NOPTS_VALUE) {
                out.maxGapMs = std::max(out.maxGapMs, (pkt->pts - last - lastDur) * av_q2d(tb) * 1000.0);
            }
            last = pkt->pts;
            lastDur = pkt->duration;
            avcodec_send_packet(dec, pkt);
            drain();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, nullptr);
    drain();
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return out;
}

}  // namespace

TG_TEST(FileAudio, ProbeRejectsBadFiles)
{
    std::string why;
    TG_EXPECT_EQ(capture::FileAudioSource::Probe(TmpPath("tg_no_such_file.m4a"), &why), int{TGErr_InvalidArgument});
    TG_EXPECT(!why.empty());
    const std::string junk = TmpPath("tg_junk_audio.flv");
    {
        FILE* f = std::fopen(junk.c_str(), "wb");
        TG_ASSERT(f);
        std::fputs("definitely not a media file", f);
        std::fclose(f);
    }
    TG_EXPECT(capture::FileAudioSource::Probe(junk) != TGErr_OK);
    std::remove(junk.c_str());

    const std::string ok = TmpPath("tg_probe_ok.flv");
    TG_ASSERT(WriteSineAac(ok, 440, 0.3, 44100));
    TG_EXPECT_EQ(capture::FileAudioSource::Probe(ok), int{TGErr_OK});
    std::remove(ok.c_str());
}

TG_TEST(FileAudio, RealtimePacingAndContent)
{
    const std::string path = TmpPath("tg_file_audio_1k.flv");
    TG_ASSERT(WriteSineAac(path, 1000, 2.0, 44100));   // 44.1k 源 → 48k 输出：同时验证重采样
    capture::FileAudioSource src(path, false);
    Collector c;
    c.Attach(src);
    TG_ASSERT(src.Open(kRate, kCh) == TGErr_OK);
    TG_EXPECT(src.DurationUs() > 1900000 && src.DurationUs() < 2200000);
    const int64_t t0 = core::MonotonicClock::NowUs();
    TG_ASSERT(src.StartCapture());
    std::this_thread::sleep_for(600ms);
    src.StopCapture();
    const double elapsedSec = (core::MonotonicClock::NowUs() - t0) / 1e6;

    std::lock_guard<std::mutex> lock(c.mutex);
    const size_t samples = c.pcm.size() / kCh;
    std::printf("    samples=%zu (%.3fs, wall %.3fs) chunks=%zu\n", samples, samples / double(kRate), elapsedSec,
                c.sizes.size());
    // 实时节奏：送出的时长与墙钟一致（±40ms）
    TG_EXPECT(std::fabs(samples / double(kRate) - elapsedSec) < 0.04);
    // 每块 10ms
    TG_EXPECT(std::all_of(c.sizes.begin(), c.sizes.end(), [](int n) { return n == kRate / 100; }));
    // 时间戳严格按样本时长推进
    for (size_t i = 1; i < c.pts.size(); ++i) {
        TG_EXPECT(std::llabs(c.pts[i] - c.pts[i - 1] - 10000) <= 1);
    }
    // 内容：跳过开头 50ms（AAC 起始预滚），主频约 1kHz，幅度约 0.4 FS（RMS ≈ 0.283 × 32767）
    const size_t b = kRate / 20;
    const double f = ZeroCrossFreq(c.pcm, kCh, kRate, b, samples);
    const double rms = Rms(c.pcm, kCh, b, samples);
    std::printf("    freq=%.1fHz rms=%.0f\n", f, rms);
    TG_EXPECT(std::fabs(f - 1000.0) < 15.0);
    TG_EXPECT(rms > 7500 && rms < 11000);
    TG_EXPECT_EQ(c.eofs.load(), 0);
    std::remove(path.c_str());
}

TG_TEST(FileAudio, LoopIsSeamlessAndEofOutputsSilence)
{
    const std::string path = TmpPath("tg_file_audio_short.flv");
    TG_ASSERT(WriteSineAac(path, 500, 0.25, kRate));

    // 循环：0.25s 的文件放 0.9s → 至少循环 3 次，整段都有声
    {
        capture::FileAudioSource src(path, true);
        Collector c;
        c.Attach(src);
        TG_ASSERT(src.Open(kRate, kCh) == TGErr_OK);
        TG_ASSERT(src.StartCapture());
        std::this_thread::sleep_for(900ms);
        src.StopCapture();
        TG_EXPECT(src.Loops() >= 3u);
        TG_EXPECT(!src.Ended());
        TG_EXPECT_EQ(c.eofs.load(), 0);
        std::lock_guard<std::mutex> lock(c.mutex);
        const size_t samples = c.pcm.size() / kCh;
        // 0.1s 一段检查能量：没有任何一段是静音（循环衔接不插空白）
        int silentWindows = 0;
        for (size_t w = kRate / 20; w + kRate / 10 <= samples; w += kRate / 10) {
            silentWindows += Rms(c.pcm, kCh, w, w + kRate / 10) < 3000;
        }
        TG_EXPECT_EQ(silentWindows, 0);
    }
    // 不循环：播完通知一次 EOF，之后照常按节拍出静音
    {
        capture::FileAudioSource src(path, false);
        Collector c;
        c.Attach(src);
        TG_ASSERT(src.Open(kRate, kCh) == TGErr_OK);
        TG_ASSERT(src.StartCapture());
        std::this_thread::sleep_for(700ms);
        src.StopCapture();
        TG_EXPECT(src.Ended());
        TG_EXPECT_EQ(c.eofs.load(), 1);
        std::lock_guard<std::mutex> lock(c.mutex);
        const size_t samples = c.pcm.size() / kCh;
        TG_EXPECT(samples > static_cast<size_t>(kRate) * 6 / 10);   // EOF 后仍在出帧
        TG_EXPECT(Rms(c.pcm, kCh, kRate / 20, kRate / 5) > 7000);    // 前段有声
        TG_EXPECT(Rms(c.pcm, kCh, kRate * 4 / 10, samples) < 1.0);   // 0.4s 之后静音
    }
    // Close 后重新 Open：从头再来
    {
        capture::FileAudioSource src(path, false);
        Collector c;
        c.Attach(src);
        TG_ASSERT(src.Open(kRate, kCh) == TGErr_OK);
        src.Close();
        TG_ASSERT(src.Open(kRate, kCh) == TGErr_OK);
        TG_ASSERT(src.StartCapture());
        std::this_thread::sleep_for(150ms);
        src.StopCapture();
        std::lock_guard<std::mutex> lock(c.mutex);
        TG_EXPECT(Rms(c.pcm, kCh, kRate / 20, c.pcm.size() / kCh) > 7000);
    }
    std::remove(path.c_str());
}

TG_TEST(FileAudio, FacadeValidation)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    AudioSourceDesc d;
    d.kind = AudioSourceKind::File;
    TG_EXPECT_EQ(c.AddAudioSource(d), int{TGErr_InvalidArgument});   // 缺路径
    d.filePath = TmpPath("tg_no_such_file.m4a");
    TG_EXPECT_EQ(c.AddAudioSource(d), int{TGErr_InvalidArgument});   // 不存在：同步报错
    const std::string path = TmpPath("tg_file_audio_facade.flv");
    TG_ASSERT(WriteSineAac(path, 440, 0.3, 44100));
    d.filePath = path;
    std::string id;
    TG_EXPECT_EQ(c.AddAudioSource(d, &id), int{TGErr_OK});
    TG_EXPECT_EQ(id, std::string("file"));
    TG_EXPECT_EQ(c.AddAudioSource(d, &id), int{TGErr_OK});
    TG_EXPECT_EQ(id, std::string("file.2"));
    TG_EXPECT_EQ(c.SetAudioSourceVolume("file", 0.5f), int{TGErr_OK});
    TG_EXPECT_EQ(c.RemoveAudioSource("file"), int{TGErr_OK});
    TG_EXPECT_EQ(c.RemoveAudioSource("file.2"), int{TGErr_OK});
    std::remove(path.c_str());
}

TG_TEST(FileAudio, AddWhileRunningMixesFileAndNotifiesEof)
{
    const std::string media = TmpPath("tg_file_audio_bgm.flv");
    const std::string out = TmpPath("tg_file_audio_out.flv");
    TG_ASSERT(WriteSineAac(media, 1000, 0.8, 44100));
    std::mutex mutex;
    std::vector<std::string> eofs;
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        c.SetOnNotifyCallback([&](NotifyMsgType t, const std::string& content, int64_t) {
            if (t == EsCap_Notify_LocalFile_EOF) {
                std::lock_guard<std::mutex> lock(mutex);
                eofs.push_back(content);
            }
        });
        VideoEncoderParams vp;
        vp.width = 320;
        vp.height = 240;
        vp.fps = 30;
        vp.vBitrate = 300000;
        TG_ASSERT(c.SetupVideoEncoder(vp) == TGErr_OK);
        AudioEncoderParams ap;
        ap.enabled = true;
        ap.sampleRate = kRate;
        ap.channels = kCh;
        TG_ASSERT(c.SetupAudioEncoder(ap) == TGErr_OK);
        MuxerParams mp;
        mp.muxer = "flv";
        mp.muxerOutput = out;
        TG_ASSERT(c.SetupMuxer(mp) == TGErr_OK);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);

        // 0 ~ 0.6s：只有视频（还没有任何音频源，音频主时钟未起，视频等它）；
        // 0.6s 加文件源（0.8s，不循环）；放到 2.2s：EOF 之后文件源出静音，音频时间轴不断
        std::vector<uint8_t> nv12(320 * 240 * 3 / 2, 100);
        const auto start = std::chrono::steady_clock::now();
        bool added = false;
        while (std::chrono::steady_clock::now() - start < 2200ms) {
            if (!added && std::chrono::steady_clock::now() - start > 600ms) {
                AudioSourceDesc d;
                d.kind = AudioSourceKind::File;
                d.id = "bgm";
                d.filePath = media;
                d.loop = false;
                TG_ASSERT(c.AddAudioSource(d) == TGErr_OK);
                added = true;
            }
            c.PushExternalVideoFrame(nv12.data(), 320, 240, 320, -1);
            std::this_thread::sleep_for(33ms);
        }
        std::vector<AudioSourceLevel> levels;
        c.GetAudioLevels(levels);
        TG_ASSERT(levels.size() == 2u);
        TG_EXPECT_EQ(levels[0].id, std::string("bgm"));
        TG_EXPECT(levels[0].peakDb < -40.0f);   // 已播完 0.8s，峰值保持按帧衰减（×0.85）
        TG_EXPECT_EQ(c.RemoveAudioSource("bgm"), int{TGErr_OK});
        c.Stop();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        TG_ASSERT(eofs.size() == 1u);
        TG_EXPECT_EQ(eofs[0], std::string("bgm"));
    }
    const DecodedAudio a = DecodeAudio(out);
    TG_ASSERT(a.rate == kRate);
    const size_t total = a.pcm.size() / kCh;
    // 找出有声区间（100ms 窗口 RMS > 3000）
    std::vector<int> loud;
    for (size_t w = 0; w + kRate / 10 <= total; w += kRate / 10) {
        loud.push_back(Rms(a.pcm, kCh, w, w + kRate / 10) > 3000);
    }
    int first = -1, last = -1, count = 0;
    for (int i = 0; i < static_cast<int>(loud.size()); ++i) {
        if (loud[i]) {
            if (first < 0) first = i;
            last = i;
            ++count;
        }
    }
    std::printf("    audio total=%.2fs loud windows=%d [%d..%d] maxGap=%.1fms\n", total / double(kRate), count, first,
                last, a.maxGapMs);
    TG_EXPECT(total > static_cast<size_t>(kRate) * 14 / 10);   // 加源之后的 1.6s 全程有音频（含 EOF 后的静音）
    TG_EXPECT(a.maxGapMs < 30.0);
    // 有声约 0.8s，且连续（中间不断）
    TG_EXPECT(count >= 6 && count <= 9);
    TG_EXPECT_EQ(last - first + 1, count);
    TG_EXPECT(last < static_cast<int>(loud.size()) - 4);   // EOF 之后是静音
    if (first >= 0) {
        const size_t b = static_cast<size_t>(first + 1) * kRate / 10;
        TG_EXPECT(std::fabs(ZeroCrossFreq(a.pcm, kCh, kRate, b, b + kRate / 5) - 1000.0) < 20.0);
    }
    std::remove(media.c_str());
    std::remove(out.c_str());
}
