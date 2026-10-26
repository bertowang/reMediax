#include "core/AVSyncClock.h"

#include <algorithm>

#include "core/Clock.h"

namespace TGCapture::core
{

// ------------------------------------------------------------------ AudioClock
void AudioClock::Reset() noexcept
{
    m_nextPtsUs.store(0);
    m_outputPtsUs.store(0);
    m_started.store(false);
}

int64_t AudioClock::ReserveUs(int64_t durationUs) noexcept
{
    const int64_t pts = m_nextPtsUs.fetch_add(durationUs > 0 ? durationUs : 0);
    m_started.store(true);
    return pts;
}

int64_t AudioClock::Reserve(int nbSamples, int sampleRate) noexcept
{
    if (nbSamples <= 0 || sampleRate <= 0) {
        return ReserveUs(0);
    }
    return ReserveUs(static_cast<int64_t>(nbSamples) * 1000000LL / sampleRate);
}

void AudioClock::NotifyOutputUs(int64_t ptsUs) noexcept
{
    const int64_t cur = m_outputPtsUs.load();
    if (ptsUs > cur) {
        m_outputPtsUs.store(ptsUs);
    }
}

void AudioClock::AdvanceTo(int64_t nextPtsUs) noexcept
{
    if (nextPtsUs < 0) {
        return;
    }
    int64_t cur = m_nextPtsUs.load();
    while (nextPtsUs > cur && !m_nextPtsUs.compare_exchange_weak(cur, nextPtsUs)) {
        // 只在前进方向更新，避免多线程下回退
    }
    m_started.store(true);
}

int64_t AudioClock::InFlightUs() const noexcept
{
    const int64_t inflight = m_nextPtsUs.load() - m_outputPtsUs.load();
    return inflight > 0 ? inflight : 0;
}

int64_t AudioClock::ReferencePtsUs() const noexcept
{
    if (!m_started.load()) {
        return kInvalidPtsUs;
    }
    return m_nextPtsUs.load() + m_compensationUs.load();
}

// ------------------------------------------------------------------ VideoPtsGenerator
void VideoPtsGenerator::Reset() noexcept
{
    m_currentPts = kInvalidPtsUs;
    m_prevAudioRef = kInvalidPtsUs;
    m_prevSysUs = 0;
    m_stallFrames = 0;
    m_clampFill = 0;
    m_avDiffUs.store(0);
    m_clamped.store(0);
    m_forceSynced.store(0);
    m_adapted.store(0);
    m_stalled.store(0);
    m_skipped.store(0);
    m_jumpWarns.store(0);
    m_staleResets.store(0);
}

VideoPtsGenerator::Stats VideoPtsGenerator::stats() const noexcept
{
    Stats s;
    s.avDiffUs = m_avDiffUs.load();
    s.clamped = m_clamped.load();
    s.forceSynced = m_forceSynced.load();
    s.adapted = m_adapted.load();
    s.stalled = m_stalled.load();
    s.skipped = m_skipped.load();
    s.jumpWarns = m_jumpWarns.load();
    s.staleResets = m_staleResets.load();
    return s;
}

// 补帧的渐进追赶/减速步长（千分比）。注释里的分档来自 MediaX 的实测取值。
int64_t VideoPtsGenerator::StepFactorPermille(int64_t avDiffUs) noexcept
{
    // avDiff > 0：视频落后音频 → 加速追赶
    if (avDiffUs > 500000) {
        return 1300;   // +30%
    }
    if (avDiffUs > 300000) {
        return 1200;   // +20%
    }
    if (avDiffUs > 150000) {
        return 1100;   // +10%
    }
    // avDiff < 0：视频超前音频 → 减速等待
    if (avDiffUs < -500000) {
        return 700;    // -30%
    }
    if (avDiffUs < -300000) {
        return 800;    // -20%
    }
    if (avDiffUs < -150000) {
        return 900;    // -10%
    }
    return 1000;       // ±150ms 内正常步长
}

// clamp 后仍必须单调递增：否则下游"PTS 回退即丢帧"的保护会连续丢帧
int64_t VideoPtsGenerator::ClampMonotonic(int64_t clampedPts, int64_t prevPts, int64_t frameIntervalUs) noexcept
{
    int64_t minStepUs = frameIntervalUs / 10;
    if (minStepUs < 1000) {
        minStepUs = 1000;
    }
    return clampedPts <= prevPts ? prevPts + minStepUs : clampedPts;
}

int64_t VideoPtsGenerator::Next(int64_t audioRefPts, int64_t frameIntervalUs, bool isRealFrame)
{
    const int64_t intervalUs = frameIntervalUs > 0 ? frameIntervalUs : 33333;

    // 距上次调用的真实经过时间（只用于"音频停滞"分支），并做异常保护：
    // 系统时钟回退 / 跳变（>10 帧）时一律退回用帧间隔，避免把异常带进时间轴
    const int64_t nowUs = MonotonicClock::NowUs();
    int64_t passUs = intervalUs;
    if (m_prevSysUs != 0) {
        const int64_t delta = nowUs - m_prevSysUs;
        if (delta > 0 && delta <= 10 * intervalUs) {
            passUs = delta;
        }
    }
    m_prevSysUs = nowUs;

    // 音频时钟不可用：不猜，交给调用方处理（丢帧或走纯视频时间轴）
    if (audioRefPts < 0) {
        return kInvalidPtsUs;
    }

    // 首帧：以音频 PTS 为起点；超过 staleTimelineUs 说明是上一轮残留的时间轴，从 0 重起
    if (m_currentPts == kInvalidPtsUs) {
        if (audioRefPts > m_params.staleTimelineUs) {
            m_currentPts = 0;
            m_staleResets.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_currentPts = audioRefPts;
        }
        m_prevAudioRef = audioRefPts;
        return m_currentPts;
    }

    // 音频 PTS 回退 / 跳变告警
    // 对齐 MediaX：audio ref 倒走时**拒绝消费**本帧的新 ref —— 直接沿用上一次 ref 做追赶/pause 判定，
    // 避免 ForceAlign 把视频 PTS 一并拖后（后续 ClampMonotonic 虽兜住下界，但 avDiff 已失准）。
    const int64_t audioStep = audioRefPts - m_prevAudioRef;
    if (m_prevAudioRef >= 0 &&
        (audioStep < 0 || audioStep > m_params.jumpWarnFactor * intervalUs)) {
        m_jumpWarns.fetch_add(1, std::memory_order_relaxed);
        if (audioStep < 0) {
            audioRefPts = m_prevAudioRef;
        }
    }

    // 音频是否"停滞"不能只看严格相等：音频主时钟按 1024 样本离散推进，
    // 高帧率下相邻视频帧之间它常常没动。只有连续 stallFrames 拍都不推进才算真的停滞。
    if (audioStep > 0) {
        m_stallFrames = 0;
    } else {
        ++m_stallFrames;
    }

    if (m_stallFrames >= m_params.stallFrames) {
        // ---- Pause 档：音频停滞（静音 / 卡顿 / 源失效）----
        // 视频按系统时钟继续走，但最多超前 maxLeadUs，之后钉在 audio+maxLead 上
        m_stalled.fetch_add(1, std::memory_order_relaxed);
        const int64_t newPts = m_currentPts + passUs;
        if (newPts - audioRefPts > m_params.maxLeadUs) {
            if (!isRealFrame && (++m_clampFill) > m_params.maxConsecutiveClampFill) {
                // 补帧被连续 clamp：停止产出，避免视频 PTS 被拖着缓慢漂移
                m_skipped.fetch_add(1, std::memory_order_relaxed);
                m_prevAudioRef = audioRefPts;
                return kInvalidPtsUs;
            }
            m_currentPts = ClampMonotonic(audioRefPts + m_params.maxLeadUs, m_currentPts, intervalUs);
            m_clamped.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_clampFill = 0;
            m_currentPts = newPts;
        }
    } else {
        // ---- 音频正常增长：按偏离程度分三档 ----
        m_clampFill = 0;
        const int64_t avDiff = audioRefPts - m_currentPts;
        const int64_t absDiff = avDiff >= 0 ? avDiff : -avDiff;

        if (isRealFrame && absDiff > m_params.forceSyncFactor * intervalUs) {
            // ForceAlign 档：偏离过大，直接跳齐，避免长时间追赶累积延迟。
            // 跳齐同样不能回退，否则封装会报 "non monotonically increasing dts"
            m_currentPts = ClampMonotonic(audioRefPts, m_currentPts, intervalUs);
            m_forceSynced.fetch_add(1, std::memory_order_relaxed);
        } else if (absDiff > m_params.adaptStartFactor * intervalUs) {
            // RangeAdapt 档：中等偏离，按 ±10/20/30% 渐进追赶或减速，画面不突跳
            m_currentPts += intervalUs * StepFactorPermille(avDiff) / 1000;
            m_adapted.fetch_add(1, std::memory_order_relaxed);
        } else {
            // RangeNormal 档：正常步长
            m_currentPts += intervalUs;
        }
    }

    m_prevAudioRef = audioRefPts;
    m_avDiffUs.store(audioRefPts - m_currentPts, std::memory_order_relaxed);
    return m_currentPts;
}

// ------------------------------------------------------------------ VideoOnlyTimeline
void VideoOnlyTimeline::Reset() noexcept
{
    m_baseUs = kInvalidPtsUs;
    m_lastPts = 0;
}

int64_t VideoOnlyTimeline::Next(int64_t captureUs) noexcept
{
    if (captureUs < 0) {
        return kInvalidPtsUs;
    }
    if (m_baseUs == kInvalidPtsUs) {
        m_baseUs = captureUs;
        m_lastPts = 0;
        return 0;
    }
    const int64_t pts = captureUs - m_baseUs;
    // 采集时钟异常回退时至少前进 1ms，保证单调递增
    m_lastPts = pts > m_lastPts ? pts : m_lastPts + 1000;
    return m_lastPts;
}

}  // namespace TGCapture::core
