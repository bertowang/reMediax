#include "core/AVSyncClock.h"

#include <algorithm>

#include "core/Clock.h"

namespace TGCapture::core
{

namespace
{
// 视频超前音频超过该帧数、且本帧是突发到达时丢帧（不再压缩 PTS 间隔）
constexpr int64_t kBurstAheadFrames = 2;
}  // namespace

// ------------------------------------------------------------------ AudioClock
void AudioClock::Reset() noexcept
{
    m_nextPtsUs.store(0);
    m_outputPtsUs.store(0);
    m_lastAdvanceWallUs.store(0);
    m_started.store(false);
}

int64_t AudioClock::ReserveUs(int64_t durationUs) noexcept
{
    const int64_t pts = m_nextPtsUs.fetch_add(durationUs > 0 ? durationUs : 0);
    m_started.store(true);
    // Reserve 本质也是一次时钟推进（混流器输出路径走 AdvanceTo，采集路径走 Reserve），
    // 两条路径都更新 lastAdvanceWallUs，否则 IsFresh 对纯 Reserve 使用者判活不准
    m_lastAdvanceWallUs.store(MonotonicClock::NowUs());
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
    bool advanced = false;
    while (nextPtsUs > cur) {
        if (m_nextPtsUs.compare_exchange_weak(cur, nextPtsUs)) {
            advanced = true;
            break;
        }
        // CAS 失败时 cur 已被更新为最新值，继续比较
    }
    m_started.store(true);
    // 只有真正把时钟推前过才算"活跃"；如果 AdvanceTo 的 nextPts 比当前值还小被忽略，
    // 不应该顺便刷新墙钟（否则会把"停滞但调用者仍在尝试推进"误判为活跃）
    if (advanced) {
        m_lastAdvanceWallUs.store(MonotonicClock::NowUs());
    }
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

bool AudioClock::IsFresh(int64_t staleMs) const noexcept
{
    if (!m_started.load()) {
        return false;
    }
    const int64_t last = m_lastAdvanceWallUs.load();
    if (last == 0 || staleMs <= 0) {
        return false;
    }
    return (MonotonicClock::NowUs() - last) < staleMs * 1000;
}

int64_t AudioClock::IdleUs() const noexcept
{
    const int64_t last = m_lastAdvanceWallUs.load();
    if (!m_started.load() || last == 0) {
        return -1;
    }
    const int64_t idle = MonotonicClock::NowUs() - last;
    return idle > 0 ? idle : 0;
}

// ------------------------------------------------------------------ VideoPtsGenerator
void VideoPtsGenerator::Reset() noexcept
{
    m_currentPts = kInvalidPtsUs;
    m_prevAudioRef = kInvalidPtsUs;
    m_prevSysUs = 0;
    m_stallFrames = 0;
    m_clampFill = 0;
    m_adapting = false;
    m_diffEmaUs = 0;
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

// clamp 后仍必须单调递增：否则下游"PTS 回退即丢帧"的保护会连续丢帧
int64_t VideoPtsGenerator::ClampMonotonic(int64_t clampedPts, int64_t prevPts, int64_t frameIntervalUs) noexcept
{
    int64_t minStepUs = frameIntervalUs / 10;
    if (minStepUs < 1000) {
        minStepUs = 1000;
    }
    return clampedPts <= prevPts ? prevPts + minStepUs : clampedPts;
}

int64_t VideoPtsGenerator::Next(int64_t audioRefPts, int64_t frameIntervalUs, bool isRealFrame,
                                int64_t audioIdleUs)
{
    const int64_t intervalUs = frameIntervalUs > 0 ? frameIntervalUs : 33333;

    // 距上一个**已产出**帧的真实经过时间：Pause 档按它推进，正常档用它识别"突发"。
    // 系统时钟回退 / 跳变（>10 帧）时退回用帧间隔，避免把异常带进时间轴
    const int64_t nowUs = MonotonicClock::NowUs();
    int64_t passUs = intervalUs;
    if (m_prevSysUs != 0) {
        const int64_t delta = nowUs - m_prevSysUs;
        if (delta >= 0 && delta <= 10 * intervalUs) {
            passUs = delta;
        }
    }
    // 突发到达：距上一个产出帧不足 1/4 帧间隔（采集卡顿恢复后一次性灌入的积压帧）
    const bool burst = m_prevSysUs != 0 && passUs < intervalUs / 4;

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
        m_prevSysUs = nowUs;
        m_diffEmaUs = audioRefPts - m_currentPts;
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

    // 音频停滞判定：优先按墙钟（音频时钟超过 stallIdleUs 没推进）。
    // 旧实现只数"连续几个视频帧之间音频没推进"，视频突发（若干帧在几 ms 内到达）时会被误判为停滞。
    bool stalled = false;
    if (audioIdleUs >= 0) {
        stalled = audioIdleUs > m_params.stallIdleUs;
    } else {
        m_stallFrames = audioStep > 0 ? 0 : m_stallFrames + 1;
        stalled = m_stallFrames >= m_params.stallFrames;
    }

    auto skip = [&]() {
        m_skipped.fetch_add(1, std::memory_order_relaxed);
        m_prevAudioRef = audioRefPts;
        m_avDiffUs.store(audioRefPts - m_currentPts, std::memory_order_relaxed);
        return kInvalidPtsUs;
    };

    if (stalled) {
        // ---- Pause 档：音频停滞（静音 / 卡顿 / 源失效）----
        // 视频按系统时钟继续走，但最多超前 maxLeadUs，之后钉在 audio+maxLead 上
        m_stalled.fetch_add(1, std::memory_order_relaxed);
        if (burst && isRealFrame) {
            return skip();   // 突发帧按墙钟推进会挤出 <10ms 的 PTS 间隔：丢弃
        }
        const int64_t newPts = m_currentPts + passUs;
        if (newPts - audioRefPts > m_params.maxLeadUs) {
            const int64_t capPts = audioRefPts + m_params.maxLeadUs;
            if (capPts <= m_currentPts) {
                // 已经钉在上限：再出帧只能靠 ClampMonotonic 的最小步长往上爬（每帧 +帧间隔/10），
                // 音频停得够久就会越过 maxLead（实测 0.8s 爬出 36ms）。真实帧与补帧一律丢弃，
                // 等音频恢复，或由调用方判定主时钟失效后切到 wall-clock 时间轴
                m_clamped.fetch_add(1, std::memory_order_relaxed);
                return skip();
            }
            if (!isRealFrame && (++m_clampFill) > m_params.maxConsecutiveClampFill) {
                // 补帧被连续 clamp：停止产出，避免视频 PTS 被拖着缓慢漂移
                return skip();
            }
            m_currentPts = capPts;
            m_clamped.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_clampFill = 0;
            m_currentPts = newPts;
        }
        m_adapting = false;
    } else {
        // ---- 音频正常推进 ----
        m_clampFill = 0;
        // 用"本帧按标称间隔推进后"的预测 PTS 计算偏差（>0 视频落后）。
        // 旧实现用上一帧 PTS，偏差系统性偏大 1 个帧间隔，控制器会把视频推到超前 ~1 帧处才平衡
        const int64_t avDiff = audioRefPts - (m_currentPts + intervalUs);

        if (avDiff < -kBurstAheadFrames * intervalUs &&
            (burst || -avDiff > m_params.maxLeadUs) && isRealFrame) {
            // 视频超前且帧是突发到达（或超前超过 maxLead）：丢帧等音频。
            // 旧实现在这里 ForceAlign 到 audio → ClampMonotonic 只能 +帧间隔/10，挤出 <10ms 间隔
            return skip();
        }
        if (isRealFrame && avDiff > m_params.forceSyncFactor * intervalUs) {
            // ForceAlign 档：视频落后过多，直接跳齐，避免长时间追赶累积延迟
            m_currentPts = ClampMonotonic(audioRefPts, m_currentPts, intervalUs);
            m_forceSynced.fetch_add(1, std::memory_order_relaxed);
            m_adapting = false;
            m_diffEmaUs = audioRefPts - m_currentPts;
        } else {
            // 音频时钟按 1024 样本离散推进（44.1kHz 下 23ms 一跳），单帧 avDiff 自带 ±12ms 量化抖动：
            // 用 EMA（1/8）滤波后再判档，否则 1 帧门限会被抖动反复触发
            m_diffEmaUs += (avDiff - m_diffEmaUs) / 8;
            const int64_t emaAbs = m_diffEmaUs >= 0 ? m_diffEmaUs : -m_diffEmaUs;
            // 滞回：超过 adaptStart 帧进入 Adapt，回到 1/4 帧以内退出
            if (emaAbs > m_params.adaptStartFactor * intervalUs) {
                m_adapting = true;
            } else if (emaAbs < intervalUs / 4) {
                m_adapting = false;
            }
            int64_t step = intervalUs;
            const int64_t maxCorr = intervalUs * m_params.adaptMaxPermille / 1000;
            if (m_adapting) {
                // RangeAdapt 档：比例控制，每帧修正 avDiff/adaptGainFrames，限幅 ±adaptMaxPermille。
                // 修正量用原始 avDiff 而不是 EMA：EMA 有滞后，用它驱动会过冲（实测进程整体卡顿
                // 恢复后 avDiff 过冲到 -40ms，再落进死区不动）。量化抖动在 /gain 后只有 ±1ms 量级。
                const int64_t gain = m_params.adaptGainFrames > 0 ? m_params.adaptGainFrames : 15;
                const int64_t corr = std::clamp<int64_t>(avDiff / gain, -maxCorr, maxCorr);
                if (corr != 0) {
                    step += corr;
                    m_adapted.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (m_params.trimGainFrames > 0) {
                // RangeNormal 档：极小的持续微调（不计入 adapted），把 1 帧以内的残余偏差慢慢拉回 0，
                // 避免偏差停在死区里永远不收敛
                step += std::clamp<int64_t>(m_diffEmaUs / m_params.trimGainFrames, -maxCorr, maxCorr);
            }
            m_currentPts += step;
        }
    }

    m_prevAudioRef = audioRefPts;
    m_prevSysUs = nowUs;
    m_avDiffUs.store(audioRefPts - m_currentPts, std::memory_order_relaxed);
    return m_currentPts;
}

void VideoPtsGenerator::Rebase(int64_t lastOutputPts) noexcept
{
    if (lastOutputPts < 0) {
        return;
    }
    if (m_currentPts == kInvalidPtsUs || lastOutputPts > m_currentPts) {
        m_currentPts = lastOutputPts;
    }
    // 接管期间音频时钟没走，上一个 ref 已无参考意义：清掉，避免恢复首帧被当成"音频跳变"
    m_prevAudioRef = kInvalidPtsUs;
    m_prevSysUs = 0;
    m_stallFrames = 0;
    m_clampFill = 0;
    m_adapting = false;
    m_diffEmaUs = 0;
}

// ------------------------------------------------------------------ VideoOnlyTimeline
void VideoOnlyTimeline::Reset() noexcept
{
    m_baseUs = kInvalidPtsUs;
    m_lastPts = 0;
}

void VideoOnlyTimeline::Rebase(int64_t anchorPts, int64_t anchorCaptureUs) noexcept
{
    if (anchorPts < 0 || anchorCaptureUs < 0) {
        Reset();
        return;
    }
    // 之后 Next(captureUs) = anchorPts + (captureUs - anchorCaptureUs)
    m_baseUs = anchorCaptureUs - anchorPts;
    m_lastPts = anchorPts;
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
