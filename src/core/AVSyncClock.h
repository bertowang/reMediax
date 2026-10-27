// 音画同步（只解决"时间戳怎么算"，不碰采集/编码/封装的任何细节）
//
// 设计对齐 MediaX 的成熟做法（ESMediaX 的 VideoPtsManager + AudioTimestampManager，
// 多源混流与参考时钟 PI/BURST 校正在 core/AudioMixer.h），保留核心策略：
//
//   1. 主时钟 = 音频时钟，按"采样数 / 采样率"推进（不是系统时间）
//   2. 视频 PTS 由音频 PTS 反推，而不是用采集 wall clock
//      （采集时钟与音频时钟不是一个时钟域，直接混用会导致 PTS 跳变）
//   3. 三档收敛（对齐 VideoPtsManager）：
//        RangeNormal  |avDiff| <= 1 帧（EMA 滤波；滞回：超过 1 帧进入 Adapt，回到 1/4 帧内退出）→ 正常步长
//        RangeAdapt   比例控制：每帧修正 avDiff/adaptGainFrames，限幅 ±30% 帧间隔
//                     （旧实现按 150/300/500ms 分档，66.7~150ms 落在 ±0% 档 → 计数增长但从不收敛）
//        ForceAlign   > forceSyncFactor 帧：视频落后 → 直接跳齐；视频超前（采集突发）→ 丢帧等音频
//   4. Pause 档：音频停滞（按墙钟判定：音频时钟超过 stallIdleUs 没推进）→ 视频最多超前 maxLead(500ms)，
//      且任何情况下都单调递增（最小步长 = 帧间隔/10，下限 1ms）。
//      旧实现按"连续 N 个视频帧间音频没推进"判停滞，视频突发（N 帧在几 ms 内到达）会被误判，
//      Pause 档按真实间隔推进 → 出现 <10ms 的 PTS 间隔。
//   5. 音频时钟不可用时**绝不用系统时间顶替**，而是让调用方决定（丢帧或走纯视频时间轴）
//
// 本模块不依赖 ffmpeg、不依赖 Frame，可单独测试。
#ifndef TGCAPTURE_CORE_AV_SYNC_CLOCK_H
#define TGCAPTURE_CORE_AV_SYNC_CLOCK_H

#include <atomic>
#include <cstdint>

namespace TGCapture::core
{

constexpr int64_t kInvalidPtsUs = -1;

// ------------------------------------------------------------------ 音频时钟（主时钟）
// 时间轴 = 已采集的音频样本数换算成的微秒。采集侧每来一帧就 Reserve 一段时长，
// 因此 ReferencePtsUs() 表示"下一拍要播的音频所在的时间点"。
class AudioClock
{
public:
    void Reset() noexcept;

    // 给一帧音频分配起始 PTS（同时把时钟推进 durationUs）
    int64_t ReserveUs(int64_t durationUs) noexcept;
    int64_t Reserve(int nbSamples, int sampleRate) noexcept;

    // 混流/编码侧真的输出之后回调：把时钟推进到该 PTS（单调，不回退）
    void NotifyOutputUs(int64_t ptsUs) noexcept;
    void AdvanceTo(int64_t nextPtsUs) noexcept;

    // 人为补偿（编码+封装延迟）：让视频略微超前音频，抵消下游延迟
    void SetCompensationUs(int64_t us) noexcept { m_compensationUs.store(us); }

    int64_t NextPtsUs() const noexcept { return m_nextPtsUs.load(); }
    int64_t OutputPtsUs() const noexcept { return m_outputPtsUs.load(); }
    int64_t InFlightUs() const noexcept;
    // 视频用的参考 PTS = 下一拍音频 + 补偿
    int64_t ReferencePtsUs() const noexcept;

    bool Valid() const noexcept { return m_started.load(); }

    // "最近 staleMs 毫秒内有过 AdvanceTo" ——
    // Valid() 是单向开关（音频第一次产出后永远 true），不足以识别"音频断流"场景；
    // 由本接口按墙钟判定主时钟是否仍活跃，供 OnVideoFrame 做 fallback 决策（对齐 MediaX
    // 的 pcmSysDrift 判活机制：主时钟推进相对系统时钟延迟过久即视为失效）。
    bool IsFresh(int64_t staleMs) const noexcept;
    // 距最近一次真实推进的墙钟时长（us）；从未推进返回 -1。供 VideoPtsGenerator 判"音频停滞"
    int64_t IdleUs() const noexcept;

private:
    std::atomic<int64_t> m_nextPtsUs{0};
    std::atomic<int64_t> m_outputPtsUs{0};
    std::atomic<int64_t> m_compensationUs{0};
    std::atomic<int64_t> m_lastAdvanceWallUs{0};   // 最近一次 AdvanceTo 的墙钟（us，用于 IsFresh）
    std::atomic_bool m_started{false};
};

// ------------------------------------------------------------------ 视频 PTS 生成
struct VideoPtsParams
{
    // 音频停滞时，视频最多超前的上限（MediaX: 500ms）
    int64_t maxLeadUs = 500000;
    // RangeAdapt 档：|音频 - 视频|（EMA 滤波后）超过 adaptStartFactor * 帧间隔 开始渐进追赶/减速，
    // 回到 1/4 帧间隔以内才退出（滞回，避免在门限附近反复进出）
    int64_t adaptStartFactor = 1;
    // RangeAdapt 比例增益：每帧修正 avDiff / adaptGainFrames（约 0.5s 时间常数 @30fps）
    int64_t adaptGainFrames = 15;
    // RangeAdapt 每帧最大修正量（千分比，相对帧间隔）
    int64_t adaptMaxPermille = 300;
    // RangeNormal 档持续微调增益：每帧修正 ema/trimGainFrames（120 帧 ≈ 4s 时间常数 @30fps；0 = 关）
    int64_t trimGainFrames = 120;
    // ForceAlign 档：|音频 - 视频| 超过 forceSyncFactor * 帧间隔 直接跳到音频 PTS
    // （必须先大于 adaptStartFactor，否则渐进档不会生效）
    int64_t forceSyncFactor = 6;
    // 音频 PTS 单步跳变超过 jumpWarnFactor * 帧间隔 时告警（MediaX: 5 帧）
    int64_t jumpWarnFactor = 5;
    // 补帧被连续 clamp 超过该次数就停止产出（防止视频 PTS 缓慢漂移）
    int64_t maxConsecutiveClampFill = 3;
    // 音频停滞判定（墙钟）：音频时钟超过 stallIdleUs 没推进才算停滞。
    // 混流器正常出帧间隔 ≤ 1 帧 + 等待迟到通道的 2 帧（44.1kHz 下约 70ms），取 100ms 留余量
    int64_t stallIdleUs = 100000;
    // 调用方没有提供墙钟空闲时长时（audioIdleUs < 0）的兜底：连续 stallFrames 次调用音频都没推进才算停滞
    int64_t stallFrames = 2;
    // 首帧音频 PTS 超过该值视为上一轮的残留时间轴，从 0 重新起算
    int64_t staleTimelineUs = 5000000;
};

class VideoPtsGenerator
{
public:
    explicit VideoPtsGenerator(VideoPtsParams params = {}) : m_params(params) {}

    void Reset() noexcept;
    void SetParams(const VideoPtsParams& params) noexcept { m_params = params; }

    // audioRefPts < 0 表示音频时钟不可用：返回 kInvalidPtsUs，由调用方决定怎么办
    // （MediaX 的做法是丢帧；本工程在无音频时改走采集时间轴）
    // audioIdleUs：音频时钟距最近一次推进的墙钟时长（AudioClock::IdleUs），< 0 表示未知（退回按帧计数判停滞）
    // 返回 kInvalidPtsUs 也可能是"建议丢弃本帧"（视频突发超前 / 补帧被连续 clamp），见 stats().skipped
    int64_t Next(int64_t audioRefPts, int64_t frameIntervalUs, bool isRealFrame, int64_t audioIdleUs = -1);

    struct Stats
    {
        int64_t avDiffUs = 0;        // 音频 - 视频，>0 表示视频落后
        uint64_t clamped = 0;        // 被 clamp 到 audio+maxLead 的次数
        uint64_t forceSynced = 0;    // ForceAlign：直接跳齐到音频的次数
        uint64_t adapted = 0;        // RangeAdapt：步长确实被修正的次数
        uint64_t stalled = 0;        // 判定为"音频停滞"的次数
        uint64_t skipped = 0;        // 建议丢弃的帧数（补帧连续 clamp + 视频突发超前）
        uint64_t jumpWarns = 0;      // 音频 PTS 跳变/回退告警次数
        uint64_t staleResets = 0;    // 首帧命中残留时间轴、从 0 重起的次数
    };
    // 快照返回（计数器是原子量，Stop 线程读统计时不与采集线程竞争）
    Stats stats() const noexcept;

private:
    static int64_t ClampMonotonic(int64_t clampedPts, int64_t prevPts, int64_t frameIntervalUs) noexcept;

    VideoPtsParams m_params;
    // 以下状态只由视频采集线程读写；Reset() 必须在非运行期调用
    int64_t m_currentPts = kInvalidPtsUs;
    int64_t m_prevAudioRef = kInvalidPtsUs;
    int64_t m_prevSysUs = 0;
    int64_t m_stallFrames = 0;
    int64_t m_clampFill = 0;
    bool m_adapting = false;
    int64_t m_diffEmaUs = 0;   // avDiff 的 EMA（滤掉音频时钟 23ms 离散推进带来的量化抖动）

    std::atomic<int64_t> m_avDiffUs{0};
    std::atomic<uint64_t> m_clamped{0};
    std::atomic<uint64_t> m_forceSynced{0};
    std::atomic<uint64_t> m_adapted{0};
    std::atomic<uint64_t> m_stalled{0};
    std::atomic<uint64_t> m_skipped{0};
    std::atomic<uint64_t> m_jumpWarns{0};
    std::atomic<uint64_t> m_staleResets{0};
};

// ------------------------------------------------------------------ 纯视频时间轴
// 没有音频时（--no-audio）用采集单调时钟做时间轴：首帧归零，后续按真实采集时刻。
class VideoOnlyTimeline
{
public:
    void Reset() noexcept;
    int64_t Next(int64_t captureUs) noexcept;

private:
    int64_t m_baseUs = kInvalidPtsUs;
    int64_t m_lastPts = 0;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_AV_SYNC_CLOCK_H
