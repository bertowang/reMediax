// 音画同步（只解决"时间戳怎么算"，不碰采集/编码/封装的任何细节）
//
// 设计对齐 MediaX 的成熟做法（ESMediaX 的 VideoPtsManager + AudioTimestampManager，
// 多源混流与参考时钟 PI/BURST 校正在 core/AudioMixer.h），保留核心策略：
//
//   1. 主时钟 = 音频时钟，按"采样数 / 采样率"推进（不是系统时间）
//   2. 视频 PTS 由音频 PTS 反推，而不是用采集 wall clock
//      （采集时钟与音频时钟不是一个时钟域，直接混用会导致 PTS 跳变）
//   3. 三档收敛（对齐 VideoPtsManager）：
//        RangeNormal  |avDiff| <= 2 帧      → 正常步长
//        RangeAdapt   2 ~ 6 帧              → ±10/20/30% 渐进追赶或减速
//        ForceAlign   > 6 帧                → 直接跳齐到音频 PTS
//   4. Pause 档：音频停滞（静音、卡顿、源失效）→ 视频最多超前 maxLead(500ms)，
//      且任何情况下都单调递增（最小步长 = 帧间隔/10，下限 1ms）
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

private:
    std::atomic<int64_t> m_nextPtsUs{0};
    std::atomic<int64_t> m_outputPtsUs{0};
    std::atomic<int64_t> m_compensationUs{0};
    std::atomic_bool m_started{false};
};

// ------------------------------------------------------------------ 视频 PTS 生成
struct VideoPtsParams
{
    // 音频停滞时，视频最多超前的上限（MediaX: 500ms）
    int64_t maxLeadUs = 500000;
    // RangeAdapt 档：|音频 - 视频| 超过 adaptStartFactor * 帧间隔 开始渐进追赶/减速
    int64_t adaptStartFactor = 2;
    // ForceAlign 档：|音频 - 视频| 超过 forceSyncFactor * 帧间隔 直接跳到音频 PTS
    // （必须先大于 adaptStartFactor，否则渐进档不会生效）
    int64_t forceSyncFactor = 6;
    // 音频 PTS 单步跳变超过 jumpWarnFactor * 帧间隔 时告警（MediaX: 5 帧）
    int64_t jumpWarnFactor = 5;
    // 补帧被连续 clamp 超过该次数就停止产出（防止视频 PTS 缓慢漂移）
    int64_t maxConsecutiveClampFill = 3;
    // 音频连续 stallFrames 次调用都没有推进才算"停滞"。
    // 音频主时钟按 1024 样本离散推进（44.1kHz≈23.2ms），60fps 下相邻视频帧之间
    // 音频常常"没动"，只有连续多拍都不推进才是真的停滞（静音/卡顿/源失效）
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
    int64_t Next(int64_t audioRefPts, int64_t frameIntervalUs, bool isRealFrame);

    struct Stats
    {
        int64_t avDiffUs = 0;        // 音频 - 视频，>0 表示视频落后
        uint64_t clamped = 0;        // 被 clamp 到 audio+maxLead 的次数
        uint64_t forceSynced = 0;    // ForceAlign：直接跳齐到音频的次数
        uint64_t adapted = 0;        // RangeAdapt：渐进追赶/减速的次数
        uint64_t stalled = 0;        // 判定为"音频停滞"的次数
        uint64_t skipped = 0;        // 建议丢弃的补帧数
        uint64_t jumpWarns = 0;      // 音频 PTS 跳变/回退告警次数
        uint64_t staleResets = 0;    // 首帧命中残留时间轴、从 0 重起的次数
    };
    // 快照返回（计数器是原子量，Stop 线程读统计时不与采集线程竞争）
    Stats stats() const noexcept;

private:
    static int64_t StepFactorPermille(int64_t avDiffUs) noexcept;
    static int64_t ClampMonotonic(int64_t clampedPts, int64_t prevPts, int64_t frameIntervalUs) noexcept;

    VideoPtsParams m_params;
    // 以下状态只由视频采集线程读写；Reset() 必须在非运行期调用
    int64_t m_currentPts = kInvalidPtsUs;
    int64_t m_prevAudioRef = kInvalidPtsUs;
    int64_t m_prevSysUs = 0;
    int64_t m_stallFrames = 0;
    int64_t m_clampFill = 0;

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
