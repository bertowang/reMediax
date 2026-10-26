#include "core/AudioMixer.h"

#include <algorithm>
#include <cstring>

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
// 每秒最多补/丢 5ms 样本，避免瞬间抽变音；超过阈值才触发
constexpr int64_t kCompRateLimitUsPerSec = 5000;
constexpr int64_t kCompTriggerUs = 5000;   // |累计 drift| > 5ms 才启动补偿
constexpr int64_t kCompIntervalUs = 1000000;  // 两次补偿至少间隔 1s
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
        // 系统时间回退：直接忽略这次更新
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
    if (m_prevPts < 0 && nowUs > 0) {
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

    if (ch.sampleRate != m_outSampleRate || ch.channels != m_outChannels) {
        AVChannelLayout inLayout{};
        AVChannelLayout outLayout{};
        av_channel_layout_default(&inLayout, ch.channels);
        av_channel_layout_default(&outLayout, m_outChannels);
        SwrContext* swr = nullptr;
        if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, m_outSampleRate,
                                &inLayout, AV_SAMPLE_FMT_S16, ch.sampleRate, 0, nullptr) < 0 || !swr) {
            TG_LOGW("AudioMixer: 通道 %d 的重采样上下文创建失败，按原格式混入", id);
        } else {
            swr_init(swr);
            ch.swr = swr;
        }
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
    }

    m_channels.push_back(std::move(ch));
    TG_LOGI("AudioMixer: 通道 +1 id=%d %dHz %dch（输出 %dHz %dch）",
            id, ch.sampleRate, ch.channels, m_outSampleRate, m_outChannels);
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

void AudioMixer::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& ch : m_channels) {
        ch.pcm.clear();
        ch.clock.Reset();
    }
    m_outputPtsUs = 0;
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

AudioMixer::ChannelDiag AudioMixer::ChannelDiagnostic(int id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto* ch = Find(id);
    ChannelDiag d;
    if (ch) {
        d.clock = ch->clock.stats();
        d.underrun = ch->underrun;
        d.compensationUs = ch->compensationUs;
        d.productionDropUs = ch->productionDropUs;
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
        d.clock = ch.clock.stats();
        d.underrun = ch.underrun;
        d.compensationUs = ch.compensationUs;
        d.productionDropUs = ch.productionDropUs;
        out.push_back(d);
    }
    return out;
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

void AudioMixer::Push(int id, const int16_t* data, int frames, int64_t captureUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* ch = Find(id);
    if (!ch || !data || frames <= 0) {
        return;
    }

    // 时间戳：按采样数推进 + 与系统参考时钟比对校正（PI/BURST）
    // 采集端若没填 captureUs（Frame 默认 AV_NOPTS_VALUE），这里兜底用当前单调时钟：
    // 否则 NextPts 拿到的 nowUs 恒为 0，参考时钟变成常数，drift 会被算成巨量负值，
    // 每帧都触发 ForceAlign，通道时钟统计全废。
    const int64_t nowUs = captureUs > 0 ? captureUs : MonotonicClock::NowUs();
    ch->clock.NextPts(frames, nowUs);

    // 本帧 drift 累加（取最新一次的瞬时 drift）—— 用它做样本级补偿。
    // 对齐 MediaX FrameProductionDiag：PI/BURST 只改时间戳标签，样本级补/丢处理
    // "实际产出速率 vs 系统时间"的偏差（设备丢包/调度卡顿）。两者分工互补，不重复补偿。
    ch->pendingCompUs += ch->clock.stats().driftUs;

    const int srcChannels = ch->channels;
    if (ch->swr) {
        // 采样率/声道归一到输出格式
        const int dstSamples = swr_get_out_samples(ch->swr, frames);
        std::vector<int16_t> dst(static_cast<size_t>(dstSamples) * m_outChannels);
        const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(data)};
        uint8_t* out[1] = {reinterpret_cast<uint8_t*>(dst.data())};
        const int converted = swr_convert(ch->swr, out, dstSamples, in, frames);
        if (converted > 0) {
            ch->pcm.insert(ch->pcm.end(), dst.begin(),
                           dst.begin() + static_cast<long>(converted) * m_outChannels);
        }
    } else {
        ch->pcm.insert(ch->pcm.end(), data, data + static_cast<size_t>(frames) * srcChannels);
    }

    // 样本级补/丢（限速 kCompRateLimitUsPerSec = 5ms/s）
    // 距上一次补偿不足 1s 则跳过；|累计 drift| 不够 5ms 也跳过。
    if (ch->lastCompUs == 0) {
        ch->lastCompUs = nowUs;
    } else if (nowUs - ch->lastCompUs >= kCompIntervalUs &&
               (ch->pendingCompUs > kCompTriggerUs || ch->pendingCompUs < -kCompTriggerUs)) {
        const int outChannels = m_outChannels > 0 ? m_outChannels : ch->channels;
        if (outChannels > 0 && m_outSampleRate > 0) {
            // 本次补偿量 = min(|pendingCompUs|, 5ms)，方向与 pendingCompUs 符号一致
            int64_t deltaUs = kCompRateLimitUsPerSec;
            if (deltaUs > (ch->pendingCompUs > 0 ? ch->pendingCompUs : -ch->pendingCompUs)) {
                deltaUs = ch->pendingCompUs > 0 ? ch->pendingCompUs : -ch->pendingCompUs;
            }
            const int64_t deltaSamples =
                deltaUs * static_cast<int64_t>(m_outSampleRate) / 1000000LL;
            const size_t deltaInterleaved =
                static_cast<size_t>(deltaSamples) * static_cast<size_t>(outChannels);

            if (ch->pendingCompUs > 0 && deltaInterleaved > 0) {
                // 本路慢：pcm 尾部复制样本填充（无 PCM 可复制时跳过，等下一拍）
                if (ch->pcm.size() >= static_cast<size_t>(outChannels)) {
                    const size_t srcPos = ch->pcm.size() - static_cast<size_t>(outChannels);
                    std::vector<int16_t> last(ch->pcm.begin() + static_cast<long>(srcPos),
                                              ch->pcm.end());
                    for (size_t k = 0; k < deltaInterleaved; k += last.size()) {
                        const size_t copy = std::min(last.size(), deltaInterleaved - k);
                        ch->pcm.insert(ch->pcm.end(), last.begin(), last.begin() + copy);
                    }
                    ch->compensationUs += deltaUs;
                    ch->pendingCompUs -= deltaUs;
                    ch->lastCompUs = nowUs;
                    TG_LOGD("AudioMixer: 通道 %d 补 %lldus 样本（累计补偿 %lldms）", ch->id,
                            (long long)deltaUs, (long long)(ch->compensationUs / 1000));
                }
            } else if (ch->pendingCompUs < 0 && deltaInterleaved > 0) {
                // 本路快：从 pcm 尾部丢弃等量样本（没攒够那么多就全丢）
                const size_t drop = std::min(deltaInterleaved, ch->pcm.size());
                ch->pcm.erase(ch->pcm.end() - static_cast<long>(drop), ch->pcm.end());
                const int64_t actualDropUs =
                    static_cast<int64_t>(drop / static_cast<size_t>(outChannels)) * 1000000LL /
                    static_cast<int64_t>(m_outSampleRate);
                ch->productionDropUs += actualDropUs;
                ch->pendingCompUs += actualDropUs;
                ch->lastCompUs = nowUs;
                TG_LOGD("AudioMixer: 通道 %d 丢 %lldus 样本（累计丢弃 %lldms）", ch->id,
                        (long long)actualDropUs, (long long)(ch->productionDropUs / 1000));
            }
        }
    }
}

bool AudioMixer::Mix(int frameSamples, std::shared_ptr<uint8_t>& outData, int64_t& outPtsUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_channels.empty() || m_outChannels <= 0 || frameSamples <= 0) {
        return false;
    }

    const size_t need = static_cast<size_t>(frameSamples) * static_cast<size_t>(m_outChannels);

    // 至少有一路有数据才产出（全静音不写包，避免刷无意义的音频）
    bool anyData = false;
    for (auto& ch : m_channels) {
        if (ch.pcm.size() >= need) {
            anyData = true;
            break;
        }
    }
    if (!anyData) {
        return false;
    }

    const size_t bytes = need * sizeof(int16_t);
    outData = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    auto* dst = reinterpret_cast<int16_t*>(outData.get());

    // 先累加（32 位），再饱和到 S16；按通道 gain 加权
    std::vector<int32_t> acc(need, 0);
    for (auto& ch : m_channels) {
        const float g = ch.gain;
        if (ch.pcm.size() >= need) {
            if (g == 1.0f) {
                for (size_t i = 0; i < need; ++i) acc[i] += ch.pcm[i];
            } else {
                for (size_t i = 0; i < need; ++i)
                    acc[i] += static_cast<int32_t>(ch.pcm[i] * g);
            }
            ch.pcm.erase(ch.pcm.begin(), ch.pcm.begin() + static_cast<long>(need));
        } else {
            // 该路没数据：按静音补齐（不拖住整体时间轴）
            ++ch.underrun;
            const size_t avail = ch.pcm.size() - (ch.pcm.size() % static_cast<size_t>(m_outChannels));
            if (g == 1.0f) {
                for (size_t i = 0; i < avail; ++i) acc[i] += ch.pcm[i];
            } else {
                for (size_t i = 0; i < avail; ++i)
                    acc[i] += static_cast<int32_t>(ch.pcm[i] * g);
            }
            ch.pcm.clear();
        }
    }

    for (size_t i = 0; i < need; ++i) {
        dst[i] = static_cast<int16_t>(std::clamp<int32_t>(acc[i], -32768, 32767));
    }

    // 输出时间轴：对齐 MediaX "校正值写回 pts" ——
    // 以主通道（第一个 active 通道）的 corrected PTS 为基准，让 PI/BURST 校正真正作用于下游。
    // 主通道断流（clock 没推进）时 fallback 到 m_outputPtsUs 的线性推进，保证时间轴不冻结。
    // 用首个 active 通道的 clock 当主时钟；AudioChannelClock::NextPts 首帧前返回 0，
    // 所以直接比较"是否大于已经推进到的 outputPtsUs"即可区分"没推进"和"推进了"。
    int64_t primaryCorrected = 0;
    for (const auto& ch : m_channels) {
        if (ch.active) {
            primaryCorrected = ch.clock.CurrentPtsUs();
            break;
        }
    }
    if (primaryCorrected > m_outputPtsUs) {
        outPtsUs = primaryCorrected;
        m_outputPtsUs = primaryCorrected;
    } else {
        outPtsUs = m_outputPtsUs;
    }
    if (m_outSampleRate > 0) {
        m_outputPtsUs += static_cast<int64_t>(frameSamples) * 1000000LL / m_outSampleRate;
    }
    return true;
}

}  // namespace TGCapture::core
