#include "core/AudioMixer.h"

#include <algorithm>
#include <cstring>

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
    const int64_t frameDurUs = FrameDurationUs();
    // 先按采样数推进（音频时钟的本质）
    m_currentPts += m_sampleRate > 0 ? (static_cast<int64_t>(nbSamples) * 1000000LL / m_sampleRate) : 0;

    const int64_t refPts = ReferencePtsUs(nowUs);
    const int64_t corrected = Correct(m_currentPts, refPts, frameDurUs);
    m_currentPts = corrected;
    m_prevPts = corrected;

    if (m_stats.driftUs < 0 ? -m_stats.driftUs > kJumpWarnFactor * frameDurUs
                            : m_stats.driftUs > kJumpWarnFactor * frameDurUs) {
        TG_LOGD("AudioChannelClock: drift=%lldms（阈值 %lldms）",
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

void AudioMixer::Push(int id, const int16_t* data, int frames, int64_t captureUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* ch = Find(id);
    if (!ch || !data || frames <= 0) {
        return;
    }

    // 时间戳：按采样数推进 + 与系统参考时钟比对校正（PI/BURST）
    ch->clock.NextPts(frames, captureUs > 0 ? captureUs : 0);

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
        return;
    }

    ch->pcm.insert(ch->pcm.end(), data, data + static_cast<size_t>(frames) * srcChannels);
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

    // 先累加（32 位），再饱和到 S16
    std::vector<int32_t> acc(need, 0);
    for (auto& ch : m_channels) {
        if (ch.pcm.size() >= need) {
            for (size_t i = 0; i < need; ++i) {
                acc[i] += ch.pcm[i];
            }
            ch.pcm.erase(ch.pcm.begin(), ch.pcm.begin() + static_cast<long>(need));
        } else {
            // 该路没数据：按静音补齐（不拖住整体时间轴）
            ++ch.underrun;
            const size_t avail = ch.pcm.size() - (ch.pcm.size() % static_cast<size_t>(m_outChannels));
            for (size_t i = 0; i < avail; ++i) {
                acc[i] += ch.pcm[i];
            }
            ch.pcm.clear();
        }
    }

    for (size_t i = 0; i < need; ++i) {
        dst[i] = static_cast<int16_t>(std::clamp<int32_t>(acc[i], -32768, 32767));
    }

    // 输出时间轴由输出采样率固定推进：某路掉线也不会让时间轴停住
    outPtsUs = m_outputPtsUs;
    if (m_outSampleRate > 0) {
        m_outputPtsUs += static_cast<int64_t>(frameSamples) * 1000000LL / m_outSampleRate;
    }
    return true;
}

}  // namespace TGCapture::core
