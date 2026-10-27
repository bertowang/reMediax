// AudioMixer：混音正确性与输出时间轴（不依赖设备，纯内存喂数据）
#include <cstdint>
#include <memory>
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
