// 故障判定原语（core/Health.h）：失败/恢复的节奏与断流判定。
// 这两个类决定"宿主什么时候收到错误、什么时候收到恢复"，判错比判晚更糟，所以逐条钉住。
#include "core/Health.h"
#include "tg_test.h"

using namespace TGCapture::core;

namespace
{
constexpr int64_t kMs = 1000;
}

TG_TEST(FailureTracker, SingleFailureDoesNotReport)
{
    FailureTracker t;
    t.Configure(1000 * kMs, 200 * kMs);
    TG_EXPECT(!t.OnFailure(0));   // 偶发一次失败不该惊动宿主
    TG_EXPECT(t.Failing());
    TG_EXPECT_EQ(t.Failures(), uint64_t{1});
}

TG_TEST(FailureTracker, ReportsOncePerRoundAfterThreshold)
{
    FailureTracker t;
    t.Configure(1000 * kMs, 200 * kMs);
    int reports = 0;
    for (int ms = 0; ms <= 5000; ms += 100) {
        reports += t.OnFailure(ms * kMs) ? 1 : 0;
    }
    TG_EXPECT_EQ(reports, 1);   // 连续失败 5s 只上报一次
    TG_EXPECT(t.Reported());
    TG_EXPECT_EQ(t.FailingUs(5000 * kMs), int64_t{5000 * kMs});
}

TG_TEST(FailureTracker, BufferedSuccessWithinObservationIsNotRecovery)
{
    // 回归：AVIO 带缓冲，断流后紧接着的几次写只是进缓冲区就"成功"了，
    // 当时按单次成功即判恢复，日志出现"写包已恢复"而链路其实已经死了
    FailureTracker t;
    t.Configure(1000 * kMs, 1000 * kMs);
    for (int ms = 0; ms <= 1500; ms += 100) {
        t.OnFailure(ms * kMs);
    }
    TG_EXPECT(t.Reported());
    TG_EXPECT(!t.OnSuccess(1600 * kMs));   // 观察期内
    TG_EXPECT(!t.OnSuccess(2000 * kMs));
    TG_EXPECT(t.Failing());                // 仍算故障中
    TG_EXPECT(t.OnSuccess(2700 * kMs));    // 持续成功满 1s 才算恢复
    TG_EXPECT(!t.Failing());
}

TG_TEST(FailureTracker, FailureDuringObservationRestartsIt)
{
    FailureTracker t;
    t.Configure(500 * kMs, 1000 * kMs);
    t.OnFailure(0);
    t.OnFailure(600 * kMs);               // 已上报
    TG_EXPECT(!t.OnSuccess(700 * kMs));   // 观察期从这次成功起算
    t.OnFailure(900 * kMs);               // 又失败：之前积累的观察期必须作废
    TG_EXPECT(!t.OnSuccess(1500 * kMs));  // 重新起算，而不是接着 700 继续
    TG_EXPECT(!t.OnSuccess(2400 * kMs));  // 若接着 700 算，此刻已被误判为恢复
    TG_EXPECT(t.OnSuccess(2500 * kMs));   // 自 1500 起连续成功满 1s
}

TG_TEST(FailureTracker, UnreportedRoundRecoversSilently)
{
    FailureTracker t;
    t.Configure(1000 * kMs, 100 * kMs);
    t.OnFailure(0);                      // 没到上报门限
    TG_EXPECT(!t.OnSuccess(200 * kMs));  // 观察期起点
    TG_EXPECT(!t.OnSuccess(400 * kMs));  // 观察期已满，但没上报过就不该通告"恢复"
    TG_EXPECT(!t.Failing());             // 状态静默清回正常
}

TG_TEST(FailureTracker, SuccessWhenHealthyIsNoop)
{
    FailureTracker t;
    t.Configure(1000 * kMs, 100 * kMs);
    TG_EXPECT(!t.OnSuccess(0));
    TG_EXPECT(!t.OnSuccess(5000 * kMs));
    TG_EXPECT_EQ(t.Failures(), uint64_t{0});
}

TG_TEST(FailureTracker, RoundCountersResetAcrossRounds)
{
    FailureTracker t;
    t.Configure(100 * kMs, 100 * kMs);
    t.OnFailure(0);
    t.OnFailure(200 * kMs);
    TG_EXPECT_EQ(t.RoundFailures(), uint64_t{2});
    TG_EXPECT(!t.OnSuccess(400 * kMs));   // 观察期起点
    TG_EXPECT(t.OnSuccess(500 * kMs));    // 观察期满，确认恢复
    TG_EXPECT_EQ(t.RoundFailures(), uint64_t{0});
    t.OnFailure(600 * kMs);
    TG_EXPECT_EQ(t.RoundFailures(), uint64_t{1});
    TG_EXPECT_EQ(t.Failures(), uint64_t{3});   // 累计跨轮
}

TG_TEST(StallWatch, DisabledWhenTimeoutNotPositive)
{
    StallWatch w;
    w.Configure(0);
    w.Reset(0);
    TG_EXPECT(w.Poll(100000 * kMs) == StallWatch::Transition::None);
    TG_EXPECT(!w.Stalled());
}

TG_TEST(StallWatch, StallsOnceThenRecovers)
{
    StallWatch w;
    w.Configure(3000 * kMs);
    w.Reset(0);
    TG_EXPECT(w.Poll(2000 * kMs) == StallWatch::Transition::None);
    TG_EXPECT(w.Poll(3500 * kMs) == StallWatch::Transition::Stalled);
    TG_EXPECT(w.Poll(4000 * kMs) == StallWatch::Transition::None);   // 同一次断流不重复
    TG_EXPECT(w.Stalled());
    w.OnActivity(5000 * kMs);
    TG_EXPECT(w.Poll(5100 * kMs) == StallWatch::Transition::Recovered);
    TG_EXPECT(!w.Stalled());
    TG_EXPECT_EQ(w.Stalls(), uint64_t{1});
}

TG_TEST(StallWatch, ActivityNeverMovesBaselineBackwards)
{
    // 多路音频乱序打点时，迟到的旧时间戳不能把基准拨回过去（否则会误报断流）
    StallWatch w;
    w.Configure(1000 * kMs);
    w.Reset(0);
    w.OnActivity(5000 * kMs);
    w.OnActivity(1000 * kMs);
    TG_EXPECT_EQ(w.IdleUs(5200 * kMs), int64_t{200 * kMs});
    TG_EXPECT(w.Poll(5500 * kMs) == StallWatch::Transition::None);
}

TG_TEST(StallWatch, ResetClearsStallState)
{
    StallWatch w;
    w.Configure(1000 * kMs);
    w.Reset(0);
    TG_EXPECT(w.Poll(2000 * kMs) == StallWatch::Transition::Stalled);
    w.Reset(3000 * kMs);   // 重开一轮：Prepare 到 Start 之间的空档不算断流
    TG_EXPECT(!w.Stalled());
    TG_EXPECT_EQ(w.Stalls(), uint64_t{0});
    TG_EXPECT(w.Poll(3500 * kMs) == StallWatch::Transition::None);
}

TG_TEST(StallWatch, IdleIsNeverNegative)
{
    StallWatch w;
    w.Configure(1000 * kMs);
    w.Reset(5000 * kMs);
    TG_EXPECT_EQ(w.IdleUs(4000 * kMs), int64_t{0});
}
