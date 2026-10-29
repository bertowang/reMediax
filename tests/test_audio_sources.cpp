// 运行期增删音频源（门面层）：推流中加 / 删外部 PCM 源，输出音频时间轴连续、无断档；
// 全部删除后混音输出静音、推流不中断；电平、音量、通知与参数校验
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
#include "core/Clock.h"
#include "tg_test.h"

extern "C" {
#include <libavformat/avformat.h>
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

// 10ms 正弦 PCM
std::vector<int16_t> Sine(double freq, double& phase, int16_t amp = 8000)
{
    const int n = kRate / 100;
    std::vector<int16_t> pcm(static_cast<size_t>(n) * kCh);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<int16_t>(amp * std::sin(phase));
        phase += 2.0 * M_PI * freq / kRate;
        for (int c = 0; c < kCh; ++c) pcm[static_cast<size_t>(i) * kCh + c] = v;
    }
    return pcm;
}

struct Rig
{
    TGCapturer c{GameID::GAME_ID_DEFAULT};
    std::mutex mutex;
    std::vector<std::pair<int, std::string>> notifies;   // (type, content)

    explicit Rig(const std::string& out)
    {
        c.SetOnNotifyCallback([this](NotifyMsgType t, const std::string& content, int64_t) {
            std::lock_guard<std::mutex> lock(mutex);
            notifies.emplace_back(static_cast<int>(t), content);
        });
        VideoEncoderParams vp;
        vp.width = 320;
        vp.height = 240;
        vp.fps = 30;
        vp.vBitrate = 300000;
        TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_OK});
        AudioEncoderParams ap;
        ap.enabled = true;
        ap.sampleRate = kRate;
        ap.channels = kCh;
        TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_OK});
        MuxerParams mp;
        mp.muxer = "flv";
        mp.muxerOutput = out;
        TG_EXPECT_EQ(c.SetupMuxer(mp), int{TGErr_OK});
    }

    int CountNotify(int type, const std::string& content)
    {
        std::lock_guard<std::mutex> lock(mutex);
        int n = 0;
        for (const auto& [t, s] : notifies) n += (t == type && s == content);
        return n;
    }

    // 实时节奏推 durMs：视频每 33ms 一帧，每个列出的源每 10ms 一块 PCM
    void Run(int durMs, const std::vector<std::pair<std::string, double>>& sources, std::vector<double>& phases)
    {
        std::vector<uint8_t> nv12(320 * 240 * 3 / 2, 100);
        const auto start = std::chrono::steady_clock::now();
        auto next = start;
        int tick = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(durMs)) {
            for (size_t i = 0; i < sources.size(); ++i) {
                auto pcm = Sine(sources[i].second, phases[i]);
                c.PushAudioSourceFrame(sources[i].first, reinterpret_cast<const uint8_t*>(pcm.data()),
                                       static_cast<int>(pcm.size() * sizeof(int16_t)), kRate, kCh, -1);
            }
            if (tick % 3 == 0) {
                c.PushExternalVideoFrame(nv12.data(), 320, 240, 320, -1);
            }
            ++tick;
            next += 10ms;
            std::this_thread::sleep_until(next);
        }
    }
};

struct AudioTimeline
{
    int packets = 0;
    double durationSec = 0;
    double maxGapMs = 0;   // 相邻音频包 PTS 间隔 - 包时长 的最大值
};

AudioTimeline ReadAudio(const std::string& path)
{
    AudioTimeline t;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return t;
    }
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        AVPacket* pkt = av_packet_alloc();
        int64_t first = AV_NOPTS_VALUE;
        int64_t last = AV_NOPTS_VALUE;
        int64_t lastDur = 0;
        AVRational tb{1, 1000};
        while (av_read_frame(fmt, pkt) >= 0) {
            auto* st = fmt->streams[pkt->stream_index];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && pkt->pts != AV_NOPTS_VALUE) {
                tb = st->time_base;
                if (first == AV_NOPTS_VALUE) first = pkt->pts;
                if (last != AV_NOPTS_VALUE) {
                    const double gap = (pkt->pts - last - lastDur) * av_q2d(tb) * 1000.0;
                    t.maxGapMs = std::max(t.maxGapMs, gap);
                }
                last = pkt->pts;
                lastDur = pkt->duration > 0 ? pkt->duration : 0;
                ++t.packets;
            }
            av_packet_unref(pkt);
        }
        if (first != AV_NOPTS_VALUE) t.durationSec = (last - first) * av_q2d(tb);
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    return t;
}

}  // namespace

TG_TEST(AudioSources, ArgumentValidation)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    AudioSourceDesc d;
    d.kind = AudioSourceKind::External;
    d.id = "bad id";
    TG_EXPECT_EQ(c.AddAudioSource(d), int{TGErr_InvalidArgument});
    d.id = "master";
    TG_EXPECT_EQ(c.AddAudioSource(d), int{TGErr_InvalidArgument});
    d.id = "bgm";
    d.volume = 5.0f;
    TG_EXPECT_EQ(c.AddAudioSource(d), int{TGErr_InvalidArgument});
    d.volume = 1.0f;
    std::string id;
    TG_EXPECT_EQ(c.AddAudioSource(d, &id), int{TGErr_OK});
    TG_EXPECT_EQ(id, std::string("bgm"));
    TG_EXPECT_EQ(c.AddAudioSource(d), int{TGErr_InvalidArgument});   // 重复
    // 自动命名
    AudioSourceDesc anon;
    TG_EXPECT_EQ(c.AddAudioSource(anon, &id), int{TGErr_OK});
    TG_EXPECT_EQ(id, std::string("pcm"));
    TG_EXPECT_EQ(c.AddAudioSource(anon, &id), int{TGErr_OK});
    TG_EXPECT_EQ(id, std::string("pcm.2"));

    AudioSourceDesc file;
    file.kind = AudioSourceKind::File;
    TG_EXPECT_EQ(c.AddAudioSource(file), int{TGErr_InvalidArgument});   // 缺路径

    TG_EXPECT_EQ(c.SetAudioSourceVolume("bgm", 0.5f), int{TGErr_OK});
    TG_EXPECT_EQ(c.SetAudioSourceVolume("bgm", -1.0f), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetAudioSourceVolume("nope", 1.0f), int{TGErr_InvalidArgument});
    int16_t pcm[960] = {};
    TG_EXPECT_EQ(c.PushAudioSourceFrame("nope", reinterpret_cast<uint8_t*>(pcm), sizeof(pcm), kRate, kCh, -1),
                 int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.PushAudioSourceFrame("bgm", reinterpret_cast<uint8_t*>(pcm), sizeof(pcm), kRate, kCh, -1),
                 int{TGErr_InvalidState});   // 未开播

    std::vector<AudioSourceLevel> levels;
    TG_EXPECT_EQ(c.GetAudioLevels(levels), int{TGErr_OK});
    TG_ASSERT(levels.size() == 4u);   // bgm / pcm / pcm.2 / master
    TG_EXPECT_EQ(levels.back().id, std::string("master"));
    TG_EXPECT(levels.front().peakDb <= -99.0f);

    TG_EXPECT_EQ(c.RemoveAudioSource("nope"), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.RemoveAudioSource("pcm"), int{TGErr_OK});
    TG_EXPECT_EQ(c.RemoveAudioSource("pcm"), int{TGErr_InvalidArgument});
}

TG_TEST(AudioSources, AddRemoveWhileRunningKeepsAudioContinuous)
{
    const std::string out = TmpPath("tg_test_audio_sources.flv");
    {
        Rig rig(out);
        AudioSourceDesc a;
        a.id = "a";
        TG_ASSERT(rig.c.AddAudioSource(a) == TGErr_OK);
        TG_ASSERT(rig.c.Prepare() == TGErr_OK);
        TG_ASSERT(rig.c.Start() == TGErr_OK);
        std::vector<double> phases(2, 0.0);

        rig.Run(800, {{"a", 440.0}}, phases);
        std::vector<AudioSourceLevel> levels;
        rig.c.GetAudioLevels(levels);
        TG_ASSERT(levels.size() == 2u);
        TG_EXPECT_EQ(levels[0].id, std::string("a"));
        TG_EXPECT(levels[0].peakDb > -15.0f && levels[0].peakDb < -9.0f);   // 8000/32768 ≈ -12.3dB
        TG_EXPECT(levels[1].peakDb > -15.0f);

        // 推流中加 b（音量 0.5）
        AudioSourceDesc b;
        b.id = "b";
        b.volume = 0.5f;
        TG_ASSERT(rig.c.AddAudioSource(b) == TGErr_OK);
        rig.Run(800, {{"a", 440.0}, {"b", 880.0}}, phases);
        rig.c.GetAudioLevels(levels);
        TG_ASSERT(levels.size() == 3u);
        TG_EXPECT_EQ(levels[1].id, std::string("b"));
        TG_EXPECT(levels[1].peakDb > -21.0f && levels[1].peakDb < -15.0f);   // 增益 0.5 → 约 -18.3dB

        // 推流中删 a，只剩 b
        TG_ASSERT(rig.c.RemoveAudioSource("a") == TGErr_OK);
        rig.Run(800, {{"b", 880.0}}, phases);
        // 全部删掉：混音输出静音，推流不中断
        TG_ASSERT(rig.c.RemoveAudioSource("b") == TGErr_OK);
        rig.Run(800, {}, phases);
        rig.c.GetAudioLevels(levels);
        TG_ASSERT(levels.size() == 1u);
        TG_EXPECT(levels[0].peakDb < -60.0f);
        rig.c.Stop();

        TG_EXPECT_EQ(rig.CountNotify(EsCap_Notify_AudioSource_Added, "a"), 1);
        TG_EXPECT_EQ(rig.CountNotify(EsCap_Notify_AudioSource_Added, "b"), 1);
        TG_EXPECT_EQ(rig.CountNotify(EsCap_Notify_AudioSource_Removed, "a"), 1);
        TG_EXPECT_EQ(rig.CountNotify(EsCap_Notify_AudioSource_Removed, "b"), 1);
    }
    const auto t = ReadAudio(out);
    // 3.2s 推流：音频时间轴覆盖全程（含最后 0.8s 的静音），相邻包无断档
    TG_EXPECT(t.durationSec > 2.8);
    TG_EXPECT(t.maxGapMs < 30.0);
    std::printf("    audio packets=%d duration=%.2fs maxGap=%.1fms\n", t.packets, t.durationSec, t.maxGapMs);
    std::remove(out.c_str());
}

TG_TEST(AudioSources, LegacyExternalHasIdAndCanBeRemoved)
{
    const std::string out = TmpPath("tg_test_audio_sources_legacy.flv");
    {
        Rig rig(out);
        TG_ASSERT(rig.c.Prepare() == TGErr_OK);
        TG_ASSERT(rig.c.Start() == TGErr_OK);
        double phase = 0;
        for (int i = 0; i < 20; ++i) {
            auto pcm = Sine(440, phase);
            rig.c.PushExternalAudioFrame(TypeOfGameAudio4Slave, reinterpret_cast<const uint8_t*>(pcm.data()),
                                         static_cast<int>(pcm.size() * sizeof(int16_t)), kRate, kCh, -1);
            std::this_thread::sleep_for(10ms);
        }
        std::vector<AudioSourceLevel> levels;
        rig.c.GetAudioLevels(levels);
        TG_ASSERT(levels.size() == 2u);
        TG_EXPECT_EQ(levels[0].id, std::string("ext.0"));
        TG_EXPECT_EQ(rig.c.SetAudioSourceVolume("ext.0", 0.2f), int{TGErr_OK});
        TG_EXPECT_EQ(rig.c.RemoveAudioSource("ext.0"), int{TGErr_OK});
        rig.c.GetAudioLevels(levels);
        TG_EXPECT_EQ(levels.size(), 1u);
        rig.c.Stop();
    }
    std::remove(out.c_str());
}
