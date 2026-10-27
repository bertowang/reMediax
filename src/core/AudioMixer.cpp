#include "core/AudioMixer.h"

#include <algorithm>
#include <cstring>

extern "C"
{
#include <libavutil/opt.h>
}

#include "core/Clock.h"
#include "core/Logger.h"

namespace TGCapture::core
{

namespace
{
// 校正常量（沿用 MediaX 的取值思路，按直播低延迟场景调整）
constexpr int64_t kMaxRefAdvanceUs = 2000000;  // 单次参考时钟推进上限 2s（冻结后不一次性追赶）
constexpr int64_t kForceAlignFactor = 5;       // |drift| > 5 帧时长 → 强制对齐
constexpr int64_t kBurstFactor = 3;            // |drift| > 3 帧时长 → BURST 收敛
constexpr int64_t kJumpWarnFactor = 5;

// 样本级补偿限速（对齐 MediaX ESAudioMixer 的 COMP_FORCE_RATE_LIMIT_US）：
// 每秒最多伸缩 5ms（≈0.5% 变速，不可闻）；累计偏差超过阈值才触发
constexpr int64_t kCompRateLimitUsPerSec = 5000;
constexpr int64_t kCompTriggerUs = 2000;      // |待补偿量| >= 2ms 才下发
constexpr int64_t kCompIntervalUs = 1000000;  // 两次下发至少间隔 1s（每次在 1s 输出样本内平滑完成）

// 漂移测量窗口：时间戳流逝 vs 样本时长
constexpr int64_t kDriftWindowUs = 5000000;        // 5s 一个窗口
constexpr int64_t kDriftGapToleranceUs = 100000;   // 相邻帧时间戳间隔超出帧长 100ms 视为断档
constexpr int64_t kTsRateTolPermille = 50;         // 调用方时间戳推进速率偏离墙钟 > 5% 视为不可信
constexpr int64_t kMaxPendingCompUs = 50000;       // 待补偿量上限（超出部分由欠载 / 缓冲上限吸收）

// 出帧 / 缓冲 / 输出时钟
constexpr int64_t kChannelIdleUs = 200000;  // 超过 200ms 没推数据的通道视为停推，不再等待
constexpr int64_t kMixWaitFrames = 2;       // 迟到通道最多等 2 帧（输出落后墙钟 2 帧即出帧）
constexpr int64_t kMaxBufferUs = 300000;    // 每路缓冲上限 300ms，超出一次性丢到一半（减少断点次数）
constexpr int64_t kMaxLeadUs = 200000;      // 输出时间轴最多超前墙钟 200ms（来源整体偏快 / 突发灌入）
constexpr int64_t kReanchorUs = 500000;     // 输出时间轴落后墙钟 > 500ms（全路断流恢复）时立即前移
// 短断流 / 持续偏慢：2s 窗口内出帧时刻的最小 lag 仍超过 (等待帧数 + 2) 帧 → 前移到只留 1 帧余量。
// 用"窗口最小值"而不是瞬时值：块状推送（如 100ms 一块）时瞬时 lag 周期性偏大，但块到达时 lag≈0。
constexpr int64_t kLagWindowUs = 2000000;

inline int64_t Abs64(int64_t v) { return v >= 0 ? v : -v; }
}  // namespace

// ------------------------------------------------------------------ AudioChannelClock
void AudioChannelClock::Init(int sampleRate, int channels, int64_t nowUs)
{
    m_sampleRate = sampleRate > 0 ? sampleRate : 48000;
    m_channels = channels > 0 ? channels : 2;
    m_currentPts = 0;
    m_prevPts = -1;
    m_refStartUs = nowUs;
    m_lastRefUs = -1;
    m_integralUs = 0;
    m_stats = Stats{};
}

void AudioChannelClock::Reset()
{
    m_currentPts = 0;
    m_prevPts = -1;
    m_lastRefUs = -1;
    m_integralUs = 0;
    m_stats = Stats{};
}

int64_t AudioChannelClock::FrameDurationUs() const
{
    // 以 1024 样本为典型帧长估算"一帧时长"，仅用于阈值与步进限幅
    const int64_t dur = 1024LL * 1000000LL / (m_sampleRate > 0 ? m_sampleRate : 48000);
    return dur > 0 ? dur : 21333;
}

// 基于系统单调时钟的参考 PTS（带跳变限幅，防止系统冻结后一次性追赶）
int64_t AudioChannelClock::ReferencePtsUs(int64_t nowUs)
{
    if (m_refStartUs == 0) {
        m_refStartUs = nowUs;
    }
    const int64_t expected = nowUs - m_refStartUs;
    if (m_lastRefUs < 0) {
        m_lastRefUs = expected;
        return expected;
    }
    const int64_t advance = expected - m_lastRefUs;
    if (advance > kMaxRefAdvanceUs) {
        // 系统时钟跳变/冻结：把多出来的时间"吸收"掉，后续自然收敛
        m_refStartUs += advance - kMaxRefAdvanceUs;
        ++m_stats.jumpCount;
        m_lastRefUs = expected - (advance - kMaxRefAdvanceUs);
        return m_lastRefUs;
    }
    if (advance < 0) {
        // 系统时间回退：忽略这次更新（lastRef 不前进，防止把回退量算进 drift）。
        // 之前这里静默吸收，调用方自带 PTS 回退时 jumpCount 永远为 0 —— 故障完全不可观测。
        // 只统计显著回退（> 1ms），避免 clock_gettime 微秒级抖动把 baseline jumpCount 刷花。
        if (-advance > 1000) {
            ++m_stats.jumpCount;
        }
        return m_lastRefUs;
    }
    m_lastRefUs = expected;
    return expected;
}

// BURST + PI 校正：返回校正后的 PTS
int64_t AudioChannelClock::Correct(int64_t rawPts, int64_t refPts, int64_t frameDurUs)
{
    const int64_t drift = refPts - rawPts;   // >0 本路慢了（落后参考）
    m_stats.driftUs = drift;

    const int64_t absDrift = drift >= 0 ? drift : -drift;

    // 1) 强制对齐：偏差过大（>5 帧时长）或长期无法收敛时直接拉回
    if (absDrift > kForceAlignFactor * frameDurUs) {
        ++m_stats.forceCount;
        m_integralUs = 0;
        return refPts;
    }

    // 2) BURST：偏差较大（>3 帧时长）时步进收敛，每次最多推进 1 帧时长，
    //    避免一帧内跳变过大导致音调突变
    if (absDrift > kBurstFactor * frameDurUs) {
        ++m_stats.burstCount;
        m_integralUs = 0;
        const int64_t step = (drift >= 0 ? frameDurUs : -frameDurUs);
        return rawPts + step;
    }

    // 3) PI 微调：每帧最多修正 1/4 帧时长（下限 1ms），平滑消除稳态误差
    ++m_stats.piCount;
    const int64_t maxPerFrame = (frameDurUs / 4) > 1000 ? (frameDurUs / 4) : 1000;

    // 比例项 + 积分项（积分项限幅，防止长期偏差累积成过冲）
    m_integralUs += drift / 8;
    const int64_t integralLimit = 10 * frameDurUs;
    if (m_integralUs > integralLimit) {
        m_integralUs = integralLimit;
    } else if (m_integralUs < -integralLimit) {
        m_integralUs = -integralLimit;
    }

    int64_t correction = drift / 4 + m_integralUs / 4;
    if (correction > maxPerFrame) {
        correction = maxPerFrame;
    } else if (correction < -maxPerFrame) {
        correction = -maxPerFrame;
    }

    int64_t corrected = rawPts + correction;
    // 不允许回退；也不允许一帧前进超过 3 帧时长
    if (m_prevPts >= 0) {
        if (corrected < m_prevPts + 1000) {
            corrected = m_prevPts + 1000;
        }
        if (corrected > m_prevPts + 3 * frameDurUs) {
            corrected = m_prevPts + 3 * frameDurUs;
        }
    }
    return corrected;
}

int64_t AudioChannelClock::NextPts(int nbSamples, int64_t nowUs)
{
    // 首帧：raw（采样数累计）与 ref（系统时钟）两个时钟必须同时起算。
    // 否则"AddChannel 到第一帧到达"之间的设备启动延迟（打开 WASAPI、填充缓冲，实测 ~300ms）
    // 会被整体算进 drift，首帧就触发 ForceAlign，之后 PI/BURST 的统计也全都偏移。
    // 注意 nowUs==0 也是合法首帧（外部注入场景常见），不能用 "> 0" 过滤。
    if (m_prevPts < 0 && nowUs >= 0) {
        m_currentPts = 0;
        m_refStartUs = nowUs;
        m_lastRefUs = 0;
        m_prevPts = 0;   // 必须置位：否则下一次调用会再次命中首帧分支，时钟永远停在 0
        return 0;
    }

    // 用本帧实际样本数换算帧长，阈值与 PI 步长才和真实节奏匹配
    const int64_t frameDurUs =
        (m_sampleRate > 0 && nbSamples > 0)
            ? (static_cast<int64_t>(nbSamples) * 1000000LL / m_sampleRate)
            : FrameDurationUs();

    // 先按采样数推进（音频时钟的本质）
    m_currentPts += m_sampleRate > 0 ? (static_cast<int64_t>(nbSamples) * 1000000LL / m_sampleRate) : 0;

    const int64_t refPts = ReferencePtsUs(nowUs);
    const int64_t corrected = Correct(m_currentPts, refPts, frameDurUs);
    m_currentPts = corrected;
    m_prevPts = corrected;

    if (m_stats.driftUs < 0 ? -m_stats.driftUs > kJumpWarnFactor * frameDurUs
                            : m_stats.driftUs > kJumpWarnFactor * frameDurUs) {
        TG_LOGW_THROTTLED(3000000, "AudioChannelClock: drift=%lldms（阈值 %lldms）",
                          static_cast<long long>(m_stats.driftUs / 1000),
                          static_cast<long long>(kJumpWarnFactor * frameDurUs / 1000));
    }
    return m_currentPts;
}

// ------------------------------------------------------------------ AudioMixer
AudioMixer::~AudioMixer()
{
    for (auto& ch : m_channels) {
        if (ch.swr) {
            swr_free(&ch.swr);
        }
    }
}

void AudioMixer::SetOutputFormat(int sampleRate, int channels)
{
    m_outSampleRate = sampleRate;
    m_outChannels = channels;
}

int AudioMixer::AddChannel(int id, int sampleRate, int channels, int64_t nowUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Find(id)) {
        return -1;
    }
    if (m_outSampleRate <= 0) {
        m_outSampleRate = sampleRate;
    }
    if (m_outChannels <= 0) {
        m_outChannels = channels;
    }

    Channel ch;
    ch.id = id;
    ch.sampleRate = sampleRate > 0 ? sampleRate : m_outSampleRate;
    ch.channels = channels > 0 ? channels : m_outChannels;
    ch.active = true;
    ch.clock.Init(ch.sampleRate, ch.channels, nowUs);

    // 重采样器始终创建（即使格式一致）：漂移补偿依赖 swr_set_compensation 平滑伸缩样本。
    // 显式打开 SWR_FLAG_RESAMPLE，否则同采样率时 swr 不建重采样器，首次 set_compensation
    // 会内部 swr_init 一次、把已缓冲的样本清掉。
    AVChannelLayout inLayout{};
    AVChannelLayout outLayout{};
    av_channel_layout_default(&inLayout, ch.channels);
    av_channel_layout_default(&outLayout, m_outChannels);
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, m_outSampleRate,
                            &inLayout, AV_SAMPLE_FMT_S16, ch.sampleRate, 0, nullptr) < 0 || !swr) {
        TG_LOGW("AudioMixer: 通道 %d 的重采样上下文创建失败（无漂移补偿）", id);
    } else {
        av_opt_set_int(swr, "swr_flags", SWR_FLAG_RESAMPLE, 0);
        if (swr_init(swr) < 0) {
            TG_LOGW("AudioMixer: 通道 %d 的重采样上下文初始化失败（无漂移补偿）", id);
            swr_free(&swr);
        } else {
            ch.swr = swr;
        }
    }
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);

    TG_LOGI("AudioMixer: 通道 +1 id=%d %dHz %dch（输出 %dHz %dch）",
            id, ch.sampleRate, ch.channels, m_outSampleRate, m_outChannels);
    m_channels.push_back(std::move(ch));
    return 0;
}

void AudioMixer::RemoveChannel(int id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        if (it->id == id) {
            if (it->swr) {
                swr_free(&it->swr);
            }
            m_channels.erase(it);
            return;
        }
    }
}

void AudioMixer::ResetChannelRuntime(Channel& ch)
{
    ch.pcm.clear();
    ch.clock.Reset();
    if (ch.swr) {
        swr_init(ch.swr);   // 清空重采样器内部缓存与未完成的补偿
    }
    ch.underrun = 0;
    ch.lastPushWallUs = 0;
    ch.winValid = false;
    ch.originTsUs = 0;
    ch.cumSamples = 0;
    ch.winStartTsUs = 0;
    ch.winStartWallUs = 0;
    ch.winOffsetSum = 0;
    ch.winCount = 0;
    ch.havePrevAvg = false;
    ch.prevAvgOffsetUs = 0;
    ch.lastTsUs = 0;
    ch.lastFrameDurUs = 0;
    ch.residualDriftUs = 0;
    ch.tsUntrusted = false;
    ch.tsAnomalies = 0;
    ch.pendingCompUs = 0;
    ch.lastCompWallUs = 0;
    ch.compensationUs = 0;
    ch.productionDropUs = 0;
    ch.overflowDropUs = 0;
}

void AudioMixer::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& ch : m_channels) {
        ResetChannelRuntime(ch);
    }
    m_outputPtsUs = 0;
    m_outputPtsBaseUs = 0;
    m_outputSamples = 0;
    m_outputStarted = false;
    m_waitStartWallUs = 0;
    m_anchorWallUs = 0;
    m_anchorPtsUs = 0;
    m_lagWinStartWallUs = 0;
    m_lagWinMinUs = 0;
    m_stats = MixerStats{};
}

void AudioMixer::ShiftOutputTimeline(int64_t shiftUs)
{
    if (shiftUs <= 0) {
        return;
    }
    m_outputPtsBaseUs = m_outputPtsUs + shiftUs;
    m_outputSamples = 0;
    m_outputPtsUs = m_outputPtsBaseUs;
    m_lagWinStartWallUs = 0;
    ++m_stats.reanchors;
    m_stats.reanchorUs += shiftUs;
    TG_LOGW_THROTTLED(3000000, "AudioMixer: 输出时间轴落后墙钟 %lldms（断流 / 来源偏慢），前移对齐（累计 %llu 次）",
                      static_cast<long long>(shiftUs / 1000),
                      static_cast<unsigned long long>(m_stats.reanchors));
}

AudioMixer::Channel* AudioMixer::Find(int id)
{
    for (auto& ch : m_channels) {
        if (ch.id == id) {
            return &ch;
        }
    }
    return nullptr;
}

const AudioMixer::Channel* AudioMixer::Find(int id) const
{
    for (const auto& ch : m_channels) {
        if (ch.id == id) {
            return &ch;
        }
    }
    return nullptr;
}

size_t AudioMixer::BufferedFrames(int id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto* ch = Find(id);
    if (!ch || m_outChannels <= 0) {
        return 0;
    }
    return ch->pcm.size() / static_cast<size_t>(m_outChannels);
}

AudioChannelClock::Stats AudioMixer::ChannelStats(int id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto* ch = Find(id);
    return ch ? ch->clock.stats() : AudioChannelClock::Stats{};
}

namespace
{
template <typename C, typename D>
void FillDiag(const C& ch, D& d)
{
    d.clock = ch.clock.stats();
    d.underrun = ch.underrun;
    d.compensationUs = ch.compensationUs;
    d.productionDropUs = ch.productionDropUs;
    d.overflowDropUs = ch.overflowDropUs;
    d.residualDriftUs = ch.residualDriftUs;
    d.tsAnomalies = ch.tsAnomalies;
    d.tsFallback = ch.tsUntrusted;
}
}  // namespace

AudioMixer::ChannelDiag AudioMixer::ChannelDiagnostic(int id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto* ch = Find(id);
    ChannelDiag d;
    if (ch) {
        FillDiag(*ch, d);
    }
    return d;
}

std::vector<AudioMixer::ChannelDiag> AudioMixer::AllDiagnostics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ChannelDiag> out;
    out.reserve(m_channels.size());
    for (const auto& ch : m_channels) {
        ChannelDiag d;
        FillDiag(ch, d);
        out.push_back(d);
    }
    return out;
}

AudioMixer::MixerStats AudioMixer::Stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void AudioMixer::SetChannelGain(int id, float gain)
{
    // 对齐 MediaX ESAudioMixer::SetChannelVolume：0 ~ 4.0，热更新不停流
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto* ch = Find(id)) {
        ch->gain = gain;
        TG_LOGI("AudioMixer: 通道 %d 增益 %.2f", id, gain);
    }
}

void AudioMixer::FallbackToWall(Channel& ch, const char* reason, int64_t valueUs)
{
    ch.tsUntrusted = true;
    ++ch.tsAnomalies;
    ch.winValid = false;
    ch.havePrevAvg = false;
    ch.clock.Reset();   // 时间戳坐标系切换，通道时钟从下一帧重新起算
    TG_LOGW("AudioMixer: 通道 %d 调用方时间戳不可信（%s，%lldus），本路改用墙钟。"
            "ptsUs 须为单调时钟、微秒、采集时刻，且严格递增（见 ITGCapturer::PushExternalAudioFrame）",
            ch.id, reason, static_cast<long long>(valueUs));
}

// 漂移测量 + 样本域补偿。返回本帧实际采用的时间戳（调用方时间戳或墙钟）。
int64_t AudioMixer::MeasureDriftAndCompensate(Channel& ch, int frames, int64_t captureUs,
                                              int64_t wallUs, bool callerTs, bool resumed)
{
    const int64_t frameDurUs = static_cast<int64_t>(frames) * 1000000LL / ch.sampleRate;

    // ---- 1) 时间戳合理性：不单调 → 本路改用墙钟 ----
    bool useCaller = callerTs && !ch.tsUntrusted;
    if (useCaller && ch.winValid && !resumed && captureUs <= ch.lastTsUs) {
        FallbackToWall(ch, "时间戳未严格递增", captureUs - ch.lastTsUs);
        useCaller = false;
    }
    const int64_t ts = useCaller ? captureUs : wallUs;

    // ---- 2) 断档检测：通道停推后恢复 / 时间戳跳空 → 重置测量原点（断流不是速率漂移）----
    bool restart = !ch.winValid || resumed;
    if (!restart && ts - ch.lastTsUs > ch.lastFrameDurUs + kDriftGapToleranceUs) {
        restart = true;
    }
    if (restart) {
        ch.winValid = true;
        ch.originTsUs = ts;
        ch.cumSamples = 0;
        ch.winStartTsUs = ts;
        ch.winStartWallUs = wallUs;
        ch.winOffsetSum = 0;
        ch.winCount = 0;
        ch.havePrevAvg = false;
    }

    // offset = 时间戳流逝 - 已推样本时长；持续增大 = 本路产样本慢于时间（需拉伸）
    const int64_t offset = (ts - ch.originTsUs) - ch.cumSamples * 1000000LL / ch.sampleRate;
    ch.winOffsetSum += offset;
    ++ch.winCount;
    ch.cumSamples += frames;
    ch.lastTsUs = ts;
    ch.lastFrameDurUs = frameDurUs;

    // ---- 3) 窗口结算：相邻窗口 offset 平均值之差 = 本窗口漂移（抖动被平均，不累积）----
    if (ts - ch.winStartTsUs >= kDriftWindowUs && ch.winCount > 0) {
        if (useCaller) {
            // 推进速率合理性：调用方时间戳流逝与墙钟流逝偏差 > 5% → 不是同一个时钟域
            const int64_t tsEl = ts - ch.winStartTsUs;
            const int64_t wallEl = wallUs - ch.winStartWallUs;
            if (wallEl > 0 && Abs64(tsEl - wallEl) * 1000 > wallEl * kTsRateTolPermille) {
                FallbackToWall(ch, "时间戳推进速率偏离墙钟 >5%", tsEl - wallEl);
                return wallUs;
            }
        }
        const int64_t avg = ch.winOffsetSum / ch.winCount;
        if (ch.havePrevAvg) {
            const int64_t drift = avg - ch.prevAvgOffsetUs;
            ch.residualDriftUs = drift;
            ch.pendingCompUs = std::clamp<int64_t>(ch.pendingCompUs + drift,
                                                   -kMaxPendingCompUs, kMaxPendingCompUs);
            if (Abs64(drift) * 1000 > (ts - ch.winStartTsUs) * 5) {
                TG_LOGW_THROTTLED(10000000, "AudioMixer: 通道 %d 漂移 %lldus/5s 超出补偿能力（0.5%%），"
                                  "超出部分由欠载补静音 / 缓冲上限丢弃吸收",
                                  ch.id, static_cast<long long>(drift));
            }
        }
        ch.prevAvgOffsetUs = avg;
        ch.havePrevAvg = true;
        ch.winStartTsUs = ts;
        ch.winStartWallUs = wallUs;
        ch.winOffsetSum = 0;
        ch.winCount = 0;
    }

    // ---- 4) 下发补偿：swr_set_compensation 在接下来 1s 输出样本内平滑伸缩，每次最多 5ms ----
    if (ch.swr && m_outSampleRate > 0 && wallUs - ch.lastCompWallUs >= kCompIntervalUs &&
        Abs64(ch.pendingCompUs) >= kCompTriggerUs) {
        const int64_t deltaUs = std::clamp<int64_t>(ch.pendingCompUs, -kCompRateLimitUsPerSec,
                                                    kCompRateLimitUsPerSec);
        const int sampleDelta = static_cast<int>(deltaUs * m_outSampleRate / 1000000LL);
        if (sampleDelta != 0 && swr_set_compensation(ch.swr, sampleDelta, m_outSampleRate) >= 0) {
            const int64_t appliedUs = static_cast<int64_t>(sampleDelta) * 1000000LL / m_outSampleRate;
            ch.pendingCompUs -= appliedUs;
            if (appliedUs > 0) {
                ch.compensationUs += appliedUs;
            } else {
                ch.productionDropUs -= appliedUs;
            }
            TG_LOGD("AudioMixer: 通道 %d 平滑补偿 %+lldus（累计拉伸 %lldms / 压缩 %lldms）", ch.id,
                    static_cast<long long>(appliedUs), static_cast<long long>(ch.compensationUs / 1000),
                    static_cast<long long>(ch.productionDropUs / 1000));
        }
        ch.lastCompWallUs = wallUs;
    }
    return ts;
}

void AudioMixer::Push(int id, const int16_t* data, int frames, int64_t captureUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* ch = Find(id);
    if (!ch || !data || frames <= 0 || ch->sampleRate <= 0) {
        return;
    }

    // 捕获时间戳：0 是合法值（外部注入首帧常为 0），只有 AV_NOPTS_VALUE / 负数才视为缺失、用墙钟兜底。
    const int64_t wallUs = MonotonicClock::NowUs();
    const bool callerTs = captureUs != AV_NOPTS_VALUE && captureUs >= 0;
    const bool resumed = ch->lastPushWallUs > 0 && wallUs - ch->lastPushWallUs > kChannelIdleUs;
    ch->lastPushWallUs = wallUs;

    const int64_t tsUs = MeasureDriftAndCompensate(*ch, frames, captureUs, wallUs, callerTs, resumed);
    // 通道时钟（PI/BURST/ForceAlign）只做诊断统计，不再写入输出 PTS
    ch->clock.NextPts(frames, tsUs);

    if (ch->swr) {
        // 采样率/声道归一到输出格式 + 漂移补偿（补偿时输出样本数会多/少几个，按 out_samples 上界分配）
        const int dstSamples = swr_get_out_samples(ch->swr, frames) + 64;
        std::vector<int16_t> dst(static_cast<size_t>(dstSamples) * m_outChannels);
        const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(data)};
        uint8_t* out[1] = {reinterpret_cast<uint8_t*>(dst.data())};
        const int converted = swr_convert(ch->swr, out, dstSamples, in, frames);
        if (converted > 0) {
            ch->pcm.insert(ch->pcm.end(), dst.begin(),
                           dst.begin() + static_cast<long>(converted) * m_outChannels);
        }
    } else if (ch->sampleRate == m_outSampleRate && ch->channels == m_outChannels) {
        ch->pcm.insert(ch->pcm.end(), data, data + static_cast<size_t>(frames) * ch->channels);
    } else {
        TG_LOGW_THROTTLED(5000000, "AudioMixer: 通道 %d 格式与输出不一致且无重采样器，丢弃", ch->id);
        return;
    }

    // 缓冲上限：超出时丢最旧数据到上限的一半（本路持续快于输出时钟 / 突发灌入时限制混流延迟；
    // 一次丢一大段而不是每次 Push 丢一点，断点次数少得多）
    if (m_outSampleRate > 0 && m_outChannels > 0) {
        const size_t cap = static_cast<size_t>(kMaxBufferUs * m_outSampleRate / 1000000LL) *
                           static_cast<size_t>(m_outChannels);
        if (ch->pcm.size() > cap) {
            size_t drop = ch->pcm.size() - cap / 2;
            drop -= drop % static_cast<size_t>(m_outChannels);
            ch->pcm.erase(ch->pcm.begin(), ch->pcm.begin() + static_cast<long>(drop));
            ch->overflowDropUs += static_cast<int64_t>(drop / static_cast<size_t>(m_outChannels)) *
                                  1000000LL / m_outSampleRate;
            TG_LOGW_THROTTLED(5000000, "AudioMixer: 通道 %d 缓冲超过 %lldms，丢弃最旧数据（累计 %lldms）",
                              ch->id, static_cast<long long>(kMaxBufferUs / 1000),
                              static_cast<long long>(ch->overflowDropUs / 1000));
        }
    }
}

bool AudioMixer::Mix(int frameSamples, std::shared_ptr<uint8_t>& outData, int64_t& outPtsUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_channels.empty() || m_outChannels <= 0 || m_outSampleRate <= 0 || frameSamples <= 0) {
        return false;
    }

    const size_t need = static_cast<size_t>(frameSamples) * static_cast<size_t>(m_outChannels);
    const int64_t nowUs = MonotonicClock::NowUs();
    const int64_t frameDurUs = static_cast<int64_t>(frameSamples) * 1000000LL / m_outSampleRate;

    // 活跃通道：kChannelIdleUs 内推过数据（从未推过 / 已停推的通道不等待，例如无声时不出包的回环采集）
    auto isLive = [&](const Channel& ch) {
        return ch.active && ch.lastPushWallUs > 0 && nowUs - ch.lastPushWallUs <= kChannelIdleUs;
    };

    bool anyReady = false;
    bool allLiveReady = true;
    for (const auto& ch : m_channels) {
        if (ch.pcm.size() >= need) {
            anyReady = true;
        } else if (isLive(ch)) {
            allLiveReady = false;
        }
    }
    if (!anyReady) {
        return false;   // 全部没数据：不产出纯静音包（断流期间时间轴空着，恢复后按墙钟前移）
    }

    // ---- 输出时钟 ----
    // lag = 墙钟流逝 - 输出时间轴流逝。>0 输出落后墙钟，<0 输出超前墙钟。
    bool timedOut = false;
    if (m_outputStarted) {
        int64_t lag = (nowUs - m_anchorWallUs) - (m_outputPtsUs - m_anchorPtsUs);
        if (lag > kReanchorUs) {
            // 全部通道长时间断流后恢复：输出时间轴前移到墙钟（只前移不回退），
            // 否则视频（由音频时钟反推）会整体滞后断流时长
            ShiftOutputTimeline(lag);
            lag = 0;
        }
        if (-lag > kMaxLeadUs) {
            // 来源整体快于墙钟 / 突发灌入：暂停出帧，多出的数据由缓冲上限丢弃
            return false;
        }
        if (!allLiveReady) {
            // 有活跃通道迟到：等到输出落后墙钟 kMixWaitFrames 帧再出帧（迟到通道本帧静音）。
            // 这样出帧节奏由墙钟兜底：慢路 / 卡顿路不会拖慢时间轴，快路也不能单独把帧数刷上去。
            if (lag < kMixWaitFrames * frameDurUs) {
                return false;
            }
            timedOut = true;
        }
        // 短断流 / 来源持续偏慢导致的稳定滞后：窗口内最小 lag 仍超过 (等待帧数 + 2) 帧 → 前移到留 1 帧余量
        if (m_lagWinStartWallUs == 0 || lag < m_lagWinMinUs) {
            m_lagWinMinUs = lag;
        }
        if (m_lagWinStartWallUs == 0) {
            m_lagWinStartWallUs = nowUs;
        } else if (nowUs - m_lagWinStartWallUs >= kLagWindowUs) {
            if (m_lagWinMinUs > (kMixWaitFrames + 2) * frameDurUs) {
                ShiftOutputTimeline(m_lagWinMinUs - frameDurUs);
            }
            m_lagWinStartWallUs = 0;   // 下一次出帧开启新窗口
        }
    } else if (!allLiveReady) {
        // 首帧：迟到通道最多等 kMixWaitFrames 帧
        if (m_waitStartWallUs == 0) {
            m_waitStartWallUs = nowUs;
        }
        if (nowUs - m_waitStartWallUs < kMixWaitFrames * frameDurUs) {
            return false;
        }
        timedOut = true;
    }
    m_waitStartWallUs = 0;
    if (timedOut) {
        ++m_stats.waitTimeouts;
    }

    const size_t bytes = need * sizeof(int16_t);
    outData = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    auto* dst = reinterpret_cast<int16_t*>(outData.get());

    // 先累加（32 位），再饱和到 S16；按通道 gain 加权
    std::vector<int32_t> acc(need, 0);
    auto accumulate = [&acc](const std::deque<int16_t>& pcm, size_t n, float g) {
        if (g == 1.0f) {
            for (size_t i = 0; i < n; ++i) acc[i] += pcm[i];
        } else {
            for (size_t i = 0; i < n; ++i) acc[i] += static_cast<int32_t>(pcm[i] * g);
        }
    };
    for (auto& ch : m_channels) {
        if (ch.pcm.size() >= need) {
            accumulate(ch.pcm, need, ch.gain);
            ch.pcm.erase(ch.pcm.begin(), ch.pcm.begin() + static_cast<long>(need));
        } else if (isLive(ch)) {
            // 活跃但迟到：本帧按静音处理，已有的残余数据保留到下一帧（不丢样本、不在帧中间插静音）
            ++ch.underrun;
        } else if (!ch.pcm.empty()) {
            // 已停推通道的尾巴：混进本帧后清空（否则恢复推流时会把陈旧数据拼到新数据前面）
            const size_t avail = ch.pcm.size() - (ch.pcm.size() % static_cast<size_t>(m_outChannels));
            accumulate(ch.pcm, avail, ch.gain);
            ch.pcm.clear();
        }
    }

    for (size_t i = 0; i < need; ++i) {
        dst[i] = static_cast<int16_t>(std::clamp<int32_t>(acc[i], -32768, 32767));
    }

    // 输出 PTS：按输出样本数线性累加（PTS 与内容严格一致）
    if (!m_outputStarted) {
        m_outputStarted = true;
        m_anchorWallUs = nowUs;
        m_anchorPtsUs = m_outputPtsUs;
    }
    outPtsUs = m_outputPtsUs;
    m_outputSamples += frameSamples;
    m_outputPtsUs = m_outputPtsBaseUs + m_outputSamples * 1000000LL / m_outSampleRate;
    ++m_stats.outputFrames;
    return true;
}

}  // namespace TGCapture::core
