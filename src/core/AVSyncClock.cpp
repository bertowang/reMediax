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
    m_clampFill = 0;
    m_stats = Stats{};
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

    // 距上次调用的真实经过时间（只用于"音频停滞"分支），并做异常保护
    const int64_t nowUs = MonotonicClock::NowUs();
    int64_t passUs = intervalUs;
    if (m_prevSysUs == 0) {
        passUs = intervalUs;
    } else {
        passUs = nowUs - m_prevSysUs;
        if (passUs < 0 || passUs > 10 * intervalUs) {
            passUs = intervalUs;   // 系统时间异常，退回用帧间隔
        }
    }
    m_prevSysUs = nowUs;

    // 音频时钟不可用：不猜，交给调用方处理
    if (audioRefPts < 0) {
        return kInvalidPtsUs;
    }

    // 首帧：直接以音频 PTS 为起点（超过 1 小时说明是残留时间轴，从 0 起）
    if (m_currentPts == kInvalidPtsUs) {
        m_currentPts = (audioRefPts > 3600LL * 1000000LL) ? 0 : audioRefPts;
        m_prevAudioRef = audioRefPts;
        return m_currentPts;
    }

    // 音频 PTS 跳变告警
    if (m_prevAudioRef >= 0) {
        const int64_t audioStep = audioRefPts - m_prevAudioRef;
        if (audioStep < 0 || audioStep > m_params.jumpWarnFactor * intervalUs) {
            ++m_stats.jumpWarns;
        }
    }

    if (audioRefPts == m_prevAudioRef) {
        // ---- 情况 A：音频 PTS 没有增长（静音 / 卡顿 / 音频源未就绪）----
        const int64_t newPts = m_currentPts + passUs;
        if (newPts - audioRefPts > m_params.maxLeadUs) {
            if (!isRealFrame && (++m_clampFill) > m_params.maxConsecutiveClampFill) {
                // 补帧被连续 clamp：停止产出，避免视频 PTS 被拖着缓慢漂移
                ++m_stats.skipped;
                m_prevAudioRef = audioRefPts;
                return kInvalidPtsUs;
            }
            m_currentPts = ClampMonotonic(audioRefPts + m_params.maxLeadUs, m_currentPts, intervalUs);
            ++m_stats.clamped;
        } else {
            m_clampFill = 0;
            m_currentPts = newPts;
        }
    } else {
        // ---- 情况 B：音频 PTS 正常增长 ----
        const int64_t avDiff = audioRefPts - m_currentPts;
        if (isRealFrame) {
            // 真实帧：偏离超过 N 个帧间隔就直接跳齐，避免长时间"追赶"造成累积延迟
            if (avDiff >= m_params.forceSyncFactor * intervalUs ||
                -avDiff >= m_params.forceSyncFactor * intervalUs) {
                // 跳齐时同样不能回退：否则封装会报
                // "non monotonically increasing dts" 并写不出文件
                m_currentPts = ClampMonotonic(audioRefPts, m_currentPts, intervalUs);
                ++m_stats.forceSynced;
            } else {
                m_currentPts += intervalUs;
            }
        } else {
            // 补帧：按偏离程度渐进加速/减速，避免画面跳变
            m_currentPts += intervalUs * StepFactorPermille(avDiff) / 1000;
        }
    }

    m_prevAudioRef = audioRefPts;
    m_stats.avDiffUs = audioRefPts - m_currentPts;
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
