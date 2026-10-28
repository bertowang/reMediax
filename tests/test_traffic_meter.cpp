// 流量计（core/TrafficMeter.h）：实时 / 累计两种口径。
// 回归背景：Muxer 原先只有一个 1s 窗口计数器，bitrateInfo.bytes 每秒清零却被当成"累计字节"输出；
// 实时码率只在写包时更新，断流后一直挂着断流前的值。
#include "core/TrafficMeter.h"
#include "tg_test.h"

using namespace TGCapture::core;

namespace
{
constexpr int64_t kMs = 1000;
constexpr int64_t kSec = 1000 * kMs;
}  // namespace

TG_TEST(TrafficMeter, EmptyIsZero)
{
    TrafficMeter m;
    TG_EXPECT_EQ(m.Bps(10 * kSec), uint64_t{0});
    TG_EXPECT_EQ(m.TotalBytes(), uint64_t{0});
    TG_EXPECT_EQ(m.AverageBps(10 * kSec), uint64_t{0});
    TG_EXPECT_EQ(m.ElapsedUs(10 * kSec), int64_t{0});
}

TG_TEST(TrafficMeter, TotalIsCumulativeAcrossWindows)
{
    // 核心回归：累计值跨窗口不清零
    TrafficMeter m;
    for (int64_t t = 0; t < 5 * kSec; t += 100 * kMs) {
        m.Add(1000, t);
    }
    TG_EXPECT_EQ(m.TotalBytes(), uint64_t{50 * 1000});
}

TG_TEST(TrafficMeter, RealtimeBpsFromClosedWindow)
{
    // 每 100ms 1000 字节 = 80 kbps
    TrafficMeter m;
    for (int64_t t = 0; t <= 3 * kSec; t += 100 * kMs) {
        m.Add(1000, t);
    }
    TG_EXPECT_EQ(m.Bps(3 * kSec), uint64_t{80000});
}

TG_TEST(TrafficMeter, NoRealtimeBeforeFirstWindowCloses)
{
    TrafficMeter m;
    m.Add(5000, 0);
    m.Add(5000, 500 * kMs);
    TG_EXPECT_EQ(m.Bps(600 * kMs), uint64_t{0});
    TG_EXPECT_EQ(m.TotalBytes(), uint64_t{10000});
}

TG_TEST(TrafficMeter, RealtimeDropsToZeroWhenStalled)
{
    // 核心回归：写出停滞后实时值不能一直挂着断流前的数
    TrafficMeter m;
    for (int64_t t = 0; t <= 2 * kSec; t += 100 * kMs) {
        m.Add(1000, t);
    }
    TG_EXPECT(m.Bps(2 * kSec) > 0);
    TG_EXPECT(m.Bps(3 * kSec) > 0);           // 停 1s：仍在容忍期内
    TG_EXPECT_EQ(m.Bps(5 * kSec), uint64_t{0});  // 停 3s：归 0
    TG_EXPECT_EQ(m.TotalBytes(), uint64_t{21 * 1000});   // 累计不受影响
}

TG_TEST(TrafficMeter, AverageCountsStallTime)
{
    // 前 2s 每秒 10000 字节，然后停 2s：平均值按 4s 计，实时值归 0
    TrafficMeter m;
    for (int64_t t = 0; t < 2 * kSec; t += 100 * kMs) {
        m.Add(1000, t);
    }
    const int64_t now = 100 * kMs + 4 * kSec;   // 第一包在 0，最后一包在 1.9s
    TG_EXPECT_EQ(m.ElapsedUs(now), now);
    TG_EXPECT_EQ(m.AverageBps(now), uint64_t{20000 * 8 * kSec / now});
    TG_EXPECT_EQ(m.Bps(now), uint64_t{0});
}

TG_TEST(TrafficMeter, BoundaryPacketCountedOnce)
{
    // 窗口边界上的包只进新窗口：[0,1s) 共 10 包，1s 处的包属于下一窗口
    TrafficMeter m;
    for (int64_t t = 0; t < kSec; t += 100 * kMs) {
        m.Add(100, t);
    }
    m.Add(100000, kSec);   // 大包落在边界
    TG_EXPECT_EQ(m.Bps(kSec), uint64_t{1000 * 8});
}

TG_TEST(TrafficMeter, ResetClearsEverything)
{
    TrafficMeter m;
    for (int64_t t = 0; t <= 2 * kSec; t += 100 * kMs) {
        m.Add(1000, t);
    }
    m.Reset();
    TG_EXPECT_EQ(m.TotalBytes(), uint64_t{0});
    TG_EXPECT_EQ(m.Bps(2 * kSec), uint64_t{0});
    TG_EXPECT_EQ(m.ElapsedUs(3 * kSec), int64_t{0});
    // 重开后时长从新的第一包算起
    m.Add(1000, 10 * kSec);
    TG_EXPECT_EQ(m.ElapsedUs(11 * kSec), int64_t{kSec});
}
