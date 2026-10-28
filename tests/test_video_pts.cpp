// VideoPtsGenerator 的离散仿真（不 sleep，毫秒级跑完）。
//
// 这几条用例对应实际踩过的坑：
//   - avDiff 用"上一帧 PTS"计算会系统性偏大一个帧间隔，基线永远不收敛到 0
//   - 视频突发到达时压缩 PTS 会挤出 <10ms 的间隔
//   - 按帧计数判"音频停滞"会把视频突发误判成停滞
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <vector>

#include "core/AVSyncClock.h"
#include "tg_test.h"

using namespace TGCapture::core;

namespace
{

constexpr double kAudioFrameUs = 1024.0 * 1e6 / 44100.0;   // 音频时钟每 1024 样本跳一次 ≈ 23.2ms
constexpr int64_t kIntervalUs = 33333;                      // 30fps

// 墙钟 tUs 时视频线程看到的音频参考：音频按 1024 样本量化推进
int64_t AudioRefAt(int64_t tUs)
{
    const int64_t seq = static_cast<int64_t>(static_cast<double>(tUs) / kAudioFrameUs);
    return static_cast<int64_t>(static_cast<double>(seq + 1) * kAudioFrameUs);
}

struct RunResult
{
    std::vector<int64_t> pts;       // 实际产出的 PTS
    std::vector<int64_t> avDiff;    // 每个产出帧之后的 avDiff
    VideoPtsGenerator::Stats stats;
};

// 视频按 arrivalUs 的间隔到达 n 帧，起始墙钟 startUs
void Feed(VideoPtsGenerator& gen, RunResult& out, int n, double arrivalUs, int64_t& tUs,
          int64_t idleUs = 5000)
{
    for (int k = 0; k < n; ++k) {
        const int64_t pts = gen.Next(AudioRefAt(tUs), kIntervalUs, true, idleUs);
        if (pts >= 0) {
            out.pts.push_back(pts);
            out.avDiff.push_back(gen.stats().avDiffUs);
        }
        tUs += static_cast<int64_t>(arrivalUs);
    }
    out.stats = gen.stats();
}

int64_t MinStep(const std::vector<int64_t>& v)
{
    int64_t m = INT64_MAX;
    for (size_t i = 1; i < v.size(); ++i) {
        m = std::min(m, v[i] - v[i - 1]);
    }
    return m;
}

int64_t MaxAbsAfter(const std::vector<int64_t>& v, size_t skip)
{
    int64_t m = 0;
    for (size_t i = skip; i < v.size(); ++i) {
        m = std::max<int64_t>(m, std::llabs(v[i]));
    }
    return m;
}

double MeanAfter(const std::vector<int64_t>& v, size_t skip)
{
    double s = 0;
    size_t n = 0;
    for (size_t i = skip; i < v.size(); ++i, ++n) {
        s += static_cast<double>(v[i]);
    }
    return n ? s / static_cast<double>(n) : 0;
}

}  // namespace

TG_TEST(VideoPts, BaselineConvergesToZeroNotToOneFrameAhead)
{
    VideoPtsGenerator gen;
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 6000, 33333.33, t);   // 视频恰好按标称帧率到达

    TG_ASSERT(r.pts.size() > 5000);
    // 回归：旧实现稳态在 -22~-42ms（超前约一帧）
    TG_EXPECT(std::abs(MeanAfter(r.avDiff, 300)) < 3000.0);
    // 仅剩音频时钟 1024 样本量化带来的 ±12ms
    TG_EXPECT(MaxAbsAfter(r.avDiff, 300) < 16000);
    TG_EXPECT_EQ(r.stats.adapted, uint64_t{0});
    TG_EXPECT_EQ(r.stats.forceSynced, uint64_t{0});
    TG_EXPECT_EQ(r.stats.skipped, uint64_t{0});
}

TG_TEST(VideoPts, BaselineStepsStayNearNominal)
{
    VideoPtsGenerator gen;
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 3000, 33333.33, t);
    TG_ASSERT(r.pts.size() > 2500);
    TG_EXPECT(MinStep(r.pts) > 25000);
    int64_t maxStep = 0;
    for (size_t i = 1; i < r.pts.size(); ++i) {
        maxStep = std::max(maxStep, r.pts[i] - r.pts[i - 1]);
    }
    TG_EXPECT(maxStep < 42000);
}

TG_TEST(VideoPts, SlightRateMismatchStaysWithinOneFrame)
{
    // 采集帧率比标称快 / 慢 1%：比例控制的稳态误差应小于一帧
    for (double arrival : {33000.0, 33666.0}) {
        VideoPtsGenerator gen;
        RunResult r;
        int64_t t = 50000;
        Feed(gen, r, 6000, arrival, t);
        TG_ASSERT(r.pts.size() > 3000);
        TG_EXPECT(std::abs(MeanAfter(r.avDiff, 600)) < static_cast<double>(kIntervalUs));
        TG_EXPECT(MinStep(r.pts) >= 10000);
    }
}

TG_TEST(VideoPts, BurstAfterStallNeverProducesTinyGaps)
{
    VideoPtsGenerator gen;
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 600, 33333.33, t);   // 正常 20s
    t += 5000000;                     // 采集卡顿 5s，期间音频照常推进
    Feed(gen, r, 150, 1000.0, t);     // 恢复后积压的 150 帧在 150ms 内灌进来
    Feed(gen, r, 600, 33333.33, t);   // 回到正常

    TG_ASSERT(r.pts.size() > 800);
    // 回归：压缩 PTS 会产生 <10ms 的间隔，封装/播放端会抖
    TG_EXPECT(MinStep(r.pts) >= 10000);
    // 严格递增
    for (size_t i = 1; i < r.pts.size(); ++i) {
        TG_EXPECT(r.pts[i] > r.pts[i - 1]);
    }
    // 突发帧是被"丢"掉的，而不是被压缩
    TG_EXPECT(r.stats.skipped > 0);
    // 恢复后重新对齐
    TG_EXPECT(std::llabs(r.avDiff.back()) < 40000);
}

TG_TEST(VideoPts, BurstIsNotMistakenForAudioStall)
{
    // 音频一直在走（idle 很小），视频突发：不能被判成 Pause 档
    VideoPtsGenerator gen;
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 300, 33333.33, t);
    const auto before = gen.stats().stalled;
    Feed(gen, r, 60, 500.0, t, /*idleUs=*/3000);
    TG_EXPECT_EQ(gen.stats().stalled, before);
}

TG_TEST(VideoPts, AudioStallKeepsMonotonicAndBoundedLead)
{
    VideoPtsGenerator gen;
    VideoPtsParams params;
    gen.SetParams(params);

    // 先正常跑一段，再让音频停滞：参考时钟冻结，墙钟空闲时长很大
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 300, 33333.33, t);
    const int64_t frozenRef = AudioRefAt(t);
    std::vector<int64_t> during;
    // Pause 档按真实墙钟推进（passUs），所以这里要真 sleep，否则会被当成突发帧丢掉
    for (int k = 0; k < 24; ++k) {   // ≈0.8s
        std::this_thread::sleep_for(std::chrono::milliseconds(34));
        const int64_t pts = gen.Next(frozenRef, kIntervalUs, true, 1000000);
        if (pts >= 0) {
            during.push_back(pts);
        }
    }
    TG_ASSERT(during.size() > 3);
    for (size_t i = 1; i < during.size(); ++i) {
        TG_EXPECT(during[i] > during[i - 1]);
    }
    // 视频最多超前 maxLead（再留一帧的量化余量）
    TG_EXPECT(during.back() <= frozenRef + params.maxLeadUs + kIntervalUs);
    TG_EXPECT(gen.stats().stalled > 0);
}

TG_TEST(VideoPts, UnavailableAudioClockReturnsInvalid)
{
    VideoPtsGenerator gen;
    TG_EXPECT_EQ(gen.Next(-1, kIntervalUs, true, 0), kInvalidPtsUs);
}

TG_TEST(VideoPts, VideoOnlyTimelineRebaseContinuesOutputPts)
{
    // 运行中音频断流：wall-clock 时间轴必须从最后一个已输出帧接续，不能从 0 重起
    VideoOnlyTimeline tl;
    tl.Rebase(2540000, 10000000);   // 最后输出 pts=2.54s，采集时刻 10.0s
    TG_EXPECT_EQ(tl.Next(10033333), int64_t{2573333});
    TG_EXPECT_EQ(tl.Next(10500000), int64_t{3040000});
    // 采集时刻回退：仍单调
    TG_EXPECT(tl.Next(10400000) > 3040000);
    // 非法锚点等价 Reset：首帧归零
    tl.Rebase(-1, 10000000);
    TG_EXPECT_EQ(tl.Next(20000000), int64_t{0});
}

TG_TEST(VideoPts, RebaseAfterWallClockTakeoverNeverGoesBackward)
{
    // 主时钟断流期间 wall-clock 接管把 PTS 推到了 5.0s；音频恢复时参考略落后（4.98s）。
    // 接回主时钟后第一帧不能比 5.0s 小（否则编码器按"PTS 回退"丢帧）
    VideoPtsGenerator gen;
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 60, 33333.33, t);   // 正常跑到 ~2s
    const int64_t lastOut = 5000000;
    gen.Rebase(lastOut);
    std::vector<int64_t> after;
    int64_t ref = 4980000;
    for (int k = 0; k < 30; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        ref += 33333;
        const int64_t pts = gen.Next(ref, kIntervalUs, true, 5000);
        if (pts >= 0) {
            after.push_back(pts);
        }
    }
    TG_ASSERT(after.size() > 20);
    TG_EXPECT(after.front() > lastOut);
    for (size_t i = 1; i < after.size(); ++i) {
        TG_EXPECT(after[i] > after[i - 1]);
    }
    // 接回后与音频的偏差应收敛在 2 帧内
    TG_EXPECT(std::llabs(gen.stats().avDiffUs) < 2 * kIntervalUs);

    // Rebase 不允许把内部 PTS 拉低
    const int64_t cur = after.back();
    gen.Rebase(1000);
    const int64_t next = gen.Next(ref + 33333, kIntervalUs, true, 5000);
    TG_EXPECT(next < 0 || next > cur);
}

TG_TEST(VideoPts, LargeLagForceAligns)
{
    VideoPtsGenerator gen;
    RunResult r;
    int64_t t = 50000;
    Feed(gen, r, 100, 33333.33, t);
    t += 2000000;   // 视频停了 2s，音频没停：落后远超 6 帧
    Feed(gen, r, 3, 33333.33, t);
    TG_EXPECT(gen.stats().forceSynced >= 1);
    TG_EXPECT(std::llabs(r.avDiff.back()) < 80000);
}
