// AudioMixer：混音正确性与输出时间轴（不依赖设备，纯内存喂数据）
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "core/AudioMixer.h"
#include "core/Clock.h"
#include "tg_test.h"

using namespace TGCapture::core;

namespace
{

constexpr int kRate = 44100;
constexpr int kCh = 2;
constexpr int kFrame = 1024;

std::vector<int16_t> Constant(int16_t value, int frames = kFrame)
{
    return std::vector<int16_t>(static_cast<size_t>(frames) * kCh, value);
}

void PushConst(AudioMixer& mixer, int id, int16_t value, int frames = kFrame)
{
    const auto pcm = Constant(value, frames);
    mixer.Push(id, pcm.data(), frames, MonotonicClock::NowUs());
}

bool MixOne(AudioMixer& mixer, std::vector<int16_t>& samples, int64_t& pts)
{
    std::shared_ptr<uint8_t> data;
    if (!mixer.Mix(kFrame, data, pts)) {
        return false;
    }
    const auto* p = reinterpret_cast<const int16_t*>(data.get());
    samples.assign(p, p + static_cast<size_t>(kFrame) * kCh);
    return true;
}

}  // namespace

TG_TEST(AudioMixer, SingleChannelPassesSamplesThrough)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    for (int i = 0; i < 4; ++i) {
        PushConst(mixer, 0, 1000);
    }
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    int mismatches = 0;
    for (int16_t s : out) {
        mismatches += (s != 1000);
    }
    TG_EXPECT_EQ(mismatches, 0);
}

TG_TEST(AudioMixer, TwoChannelsAreSummed)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    mixer.AddChannel(1, kRate, kCh, MonotonicClock::NowUs());
    for (int i = 0; i < 4; ++i) {
        PushConst(mixer, 0, 1000);
        PushConst(mixer, 1, 2000);
    }
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    int mismatches = 0;
    for (int16_t s : out) {
        mismatches += (s != 3000);
    }
    TG_EXPECT_EQ(mismatches, 0);
}

TG_TEST(AudioMixer, SumSaturatesInsteadOfWrapping)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    mixer.AddChannel(1, kRate, kCh, MonotonicClock::NowUs());
    for (int i = 0; i < 4; ++i) {
        PushConst(mixer, 0, 30000);
        PushConst(mixer, 1, 30000);
    }
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    TG_EXPECT_EQ(static_cast<int>(out[0]), 32767);   // 回绕成负数会是刺耳的爆音
    TG_EXPECT_EQ(static_cast<int>(out[1000]), 32767);
}

TG_TEST(AudioMixer, OutputPtsIsLinearInEmittedSamples)
{
    // 输出 PTS = base + 已输出样本数 / 采样率。逐帧取整累加会漂，这里要求与"总样本数"严格一致
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());

    // 混流器按墙钟限制输出超前量（kMaxLeadUs=200ms），测试不 sleep，所以只取 6 帧（≈140ms）
    std::vector<int64_t> pts;
    for (int i = 0; i < 6; ++i) {
        PushConst(mixer, 0, 500);
        PushConst(mixer, 0, 500);   // 保证始终够一帧
        std::vector<int16_t> out;
        int64_t p = 0;
        TG_ASSERT(MixOne(mixer, out, p));
        pts.push_back(p);
    }
    for (size_t i = 0; i < pts.size(); ++i) {
        const int64_t expected = pts[0] + static_cast<int64_t>(i) * kFrame * 1000000LL / kRate;
        TG_EXPECT(std::llabs(pts[i] - expected) <= 1);
    }
}

TG_TEST(AudioMixer, SilentIdleChannelDoesNotStallOutput)
{
    // 通道 1 注册了但从不推数据（设备失效 / 静音源）：不能拖住整个混流
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    mixer.AddChannel(1, kRate, kCh, MonotonicClock::NowUs());
    for (int i = 0; i < 4; ++i) {
        PushConst(mixer, 0, 700);
    }
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    TG_EXPECT_EQ(static_cast<int>(out[0]), 700);
}

TG_TEST(AudioMixer, ResetClearsTimelineAndBuffers)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    for (int i = 0; i < 4; ++i) {
        PushConst(mixer, 0, 100);
    }
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    mixer.Reset();
    TG_EXPECT_EQ(mixer.OutputPtsUs(), int64_t{0});
    TG_EXPECT_EQ(mixer.ChannelCount(), size_t{1});   // Reset 只清运行态，不摘通道
}

// ---------------------------------------------------------------- 运行期增删通道 / 电平
// 重采样器有几十个样本的延迟：每次推 2 帧，保证够出 1 帧
TG_TEST(AudioMixer, RuntimeAddedChannelFadesIn)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    std::vector<int16_t> out;
    int64_t pts = 0;
    PushConst(mixer, 0, 1000, 2 * kFrame);
    TG_ASSERT(MixOne(mixer, out, pts));
    // 输出开始后接入的通道：20ms（882 样本 @44.1k）内从 0 线性升到满增益
    mixer.AddChannel(1, kRate, kCh, MonotonicClock::NowUs());
    PushConst(mixer, 0, 1000, 2 * kFrame);
    PushConst(mixer, 1, 2000, 2 * kFrame);
    TG_ASSERT(MixOne(mixer, out, pts));
    TG_EXPECT_EQ(static_cast<int>(out[0]), 1000);                       // 首样本：新通道增益 0
    const int mid = out[static_cast<size_t>(441) * kCh];
    TG_EXPECT(mid > 1900 && mid < 2100);                                // 一半处约 +1000
    TG_EXPECT_EQ(static_cast<int>(out[static_cast<size_t>(kFrame - 1) * kCh]), 3000);   // 淡入完成
}

TG_TEST(AudioMixer, ChannelsAddedBeforeOutputDoNotFade)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    PushConst(mixer, 0, 1000, 2 * kFrame);
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    TG_EXPECT_EQ(static_cast<int>(out[0]), 1000);
}

TG_TEST(AudioMixer, FadeOutThenSilenceKeepsTimeline)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    std::vector<int16_t> out;
    int64_t pts0 = 0;
    PushConst(mixer, 0, 1000, 2 * kFrame);
    TG_ASSERT(MixOne(mixer, out, pts0));
    TG_EXPECT(!mixer.IsChannelFaded(0));

    mixer.FadeOutChannel(0);
    PushConst(mixer, 0, 1000, 2 * kFrame);
    int64_t pts1 = 0;
    TG_ASSERT(MixOne(mixer, out, pts1));
    TG_EXPECT_EQ(static_cast<int>(out[0]), 1000);
    TG_EXPECT_EQ(static_cast<int>(out[static_cast<size_t>(900) * kCh]), 0);   // 882 样本后归零
    TG_EXPECT(mixer.IsChannelFaded(0));
    mixer.RemoveChannel(0);
    TG_EXPECT_EQ(mixer.ChannelCount(), size_t{0});

    // 没有任何通道：按墙钟继续出静音帧，时间轴线性接续
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    int64_t pts2 = 0;
    TG_ASSERT(MixOne(mixer, out, pts2));
    TG_EXPECT(std::llabs(pts2 - pts1 - int64_t{kFrame} * 1000000 / kRate) <= 1);
    int nonZero = 0;
    for (int16_t v : out) nonZero += v != 0;
    TG_EXPECT_EQ(nonZero, 0);
    // 静音帧不超前墙钟：连续取，很快就取不到
    int extra = 0;
    while (MixOne(mixer, out, pts2) && extra < 50) ++extra;
    TG_EXPECT(extra < 10);
}

TG_TEST(AudioMixer, StalledEstablishedChannelStillGaps)
{
    // 已出过声的通道断流：仍然不出帧（断流看护据此上报），不能被"无源保持"掩盖
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    PushConst(mixer, 0, 1000, 2 * kFrame);
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_ASSERT(MixOne(mixer, out, pts));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    TG_EXPECT(!MixOne(mixer, out, pts));
}

TG_TEST(AudioMixer, EmptyMixerBeforeOutputProducesNothing)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    std::vector<int16_t> out;
    int64_t pts = 0;
    TG_EXPECT(!MixOne(mixer, out, pts));
}

TG_TEST(AudioMixer, LevelsPerChannelAndMaster)
{
    AudioMixer mixer;
    mixer.SetOutputFormat(kRate, kCh);
    mixer.AddChannel(0, kRate, kCh, MonotonicClock::NowUs());
    mixer.AddChannel(1, kRate, kCh, MonotonicClock::NowUs());
    mixer.SetChannelGain(1, 0.5f);
    std::vector<int16_t> out;
    int64_t pts = 0;
    for (int i = 0; i < 4; ++i) {
        PushConst(mixer, 0, 16384, 2 * kFrame);
        PushConst(mixer, 1, 8192, 2 * kFrame);
        TG_ASSERT(MixOne(mixer, out, pts));
    }
    const auto l0 = mixer.ChannelLevel(0);
    const auto l1 = mixer.ChannelLevel(1);
    const auto lm = mixer.MasterLevel();
    TG_EXPECT(std::fabs(l0.peak - 0.5f) < 0.01f);
    TG_EXPECT(std::fabs(l1.peak - 0.125f) < 0.01f);    // 增益后的电平
    TG_EXPECT(std::fabs(lm.peak - 0.625f) < 0.01f);
    TG_EXPECT(l0.rms > 0.4f && l0.rms <= 0.5f);         // RMS 平滑中，逐步逼近 0.5
    TG_EXPECT(mixer.ChannelLevel(99).peak == 0.0f);
}
