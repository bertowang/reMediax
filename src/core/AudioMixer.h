// 多源音频混流 + 时钟校正（对齐 MediaX 的 ESAudioMixer + ReferenceClockManager）
//
// 分成两块，互相独立：
//
//  1) AudioChannelClock —— 单路音频的时间戳与校正
//     - currentPts 按"采样数 / 采样率"推进（不是系统时间）
//     - 另有一个基于系统单调时钟的"参考时钟"，用来发现本路跑快/跑慢
//     - 偏差小 → PI 微调（每帧最多修正 1/4 帧时长，平滑不抖动）
//       偏差大 → BURST 步进收敛（几帧内拉回）
//       偏差极大或反向 → ForceAlign 直接对齐
//     - 系统时钟冻结/跳变（>2s）时限幅吸收，避免一次性追赶
//
//  2) AudioMixer —— N 路混成一路
//     - 每路独立缓冲，按采样点加和（32 位累加后饱和到 S16）
//     - 某路数据没到（卡顿/静音）→ 该路按静音补齐，不拖住整体时间轴
//     - 采样率/声道不一致时用 swresample 归一到输出格式
//
// 输出时间轴策略（对齐 MediaX ReferenceClockManager::CheckAndCorrectPts 的"校正值写回 pts"）：
//
//   以"主通道"（第一个加入的 active 通道）的 corrected PTS 为输出时间轴基准 ——
//   这样 PI/BURST 校正真正生效在下游（之前仅做统计，Mix 的 outPtsUs 自推进，
//   校正量完全没落到时间戳上，表现为"统计看着在收敛、实际多路漂移从没纠正"）。
//
//   主通道掉线/断流兜底：若主通道 clock 本轮未推进，fallback 到 m_outputPtsUs
//   按输出采样率线性推进，保证整体时间轴不被断流冻结。
#ifndef TGCAPTURE_CORE_AUDIO_MIXER_H
#define TGCAPTURE_CORE_AUDIO_MIXER_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

extern "C"
{
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace TGCapture::core
{

// ------------------------------------------------------------------ 单路时间戳 + PI/BURST 校正
class AudioChannelClock
{
public:
    struct Stats
    {
        int64_t driftUs = 0;      // 参考时钟 - 本路时钟（>0 本路慢了）
        uint64_t piCount = 0;     // PI 微调次数
        uint64_t burstCount = 0;  // BURST 收敛次数
        uint64_t forceCount = 0;  // 强制对齐次数
        uint64_t jumpCount = 0;   // 系统时钟跳变被限幅的次数
    };

    void Init(int sampleRate, int channels, int64_t nowUs);
    void Reset();

    // 取本帧 PTS：先按采样数推进，再与参考时钟比对并校正
    int64_t NextPts(int nbSamples, int64_t nowUs);

    [[nodiscard]] int64_t CurrentPtsUs() const { return m_currentPts; }
    [[nodiscard]] const Stats& stats() const { return m_stats; }

private:
    [[nodiscard]] int64_t FrameDurationUs() const;
    [[nodiscard]] int64_t ReferencePtsUs(int64_t nowUs);
    int64_t Correct(int64_t rawPts, int64_t refPts, int64_t frameDurUs);

    int m_sampleRate = 48000;
    int m_channels = 2;
    int64_t m_currentPts = 0;
    int64_t m_prevPts = -1;

    // 系统参考时钟（带跳变限幅）
    int64_t m_refStartUs = 0;
    int64_t m_lastRefUs = -1;

    // PI 积分项
    int64_t m_integralUs = 0;

    Stats m_stats{};
};

// ------------------------------------------------------------------ 多源混流
class AudioMixer
{
public:
    // 输出格式：第一路加入时确定；也可显式设定（编码器要求固定格式时使用）
    void SetOutputFormat(int sampleRate, int channels);
    int AddChannel(int id, int sampleRate, int channels, int64_t nowUs);
    void RemoveChannel(int id);
    void Reset();

    // 推入一路 PCM（交错 S16）
    void Push(int id, const int16_t* data, int frames, int64_t captureUs);

    // 混出 frameSamples 个样本（每声道），返回交错 S16；数据不够的通道按静音补齐
    bool Mix(int frameSamples, std::shared_ptr<uint8_t>& outData, int64_t& outPtsUs);

    [[nodiscard]] int OutputSampleRate() const { return m_outSampleRate; }
    [[nodiscard]] int OutputChannels() const { return m_outChannels; }
    [[nodiscard]] int64_t OutputPtsUs() const { return m_outputPtsUs; }
    [[nodiscard]] size_t BufferedFrames(int id) const;
    [[nodiscard]] size_t ChannelCount() const { return m_channels.size(); }

    // 各路校正统计（诊断用）
    [[nodiscard]] AudioChannelClock::Stats ChannelStats(int id) const;

    // 诊断信息：PI/BURST 统计 + 本路 underrun + 样本级补偿（对齐 MediaX FrameProductionDiag）
    struct ChannelDiag
    {
        AudioChannelClock::Stats clock{};
        uint64_t underrun = 0;              // 数据没到被静音补齐的次数
        int64_t compensationUs = 0;         // 累计少产补偿（本路慢，复制样本填充）
        int64_t productionDropUs = 0;       // 累计多产丢弃（本路快，丢弃样本）
    };
    [[nodiscard]] ChannelDiag ChannelDiagnostic(int id) const;
    // 便捷接口：按 index 顺序取全部通道诊断，供 TGCapturer::GetStatsInfo 序列化
    [[nodiscard]] std::vector<ChannelDiag> AllDiagnostics() const;

    // 运行期通道增益（线性，0.0 ~ 4.0；clamp 由 Mixer 内部完成，不停流不重建管线）
    void SetChannelGain(int id, float gain);

private:
    struct Channel
    {
        int id = -1;
        int sampleRate = 0;
        int channels = 0;
        bool active = false;
        AudioChannelClock clock;
        std::deque<int16_t> pcm;      // 交错 S16
        SwrContext* swr = nullptr;    // 采样率/声道归一到输出格式
        uint64_t underrun = 0;        // 数据没到的次数
        float gain = 1.0f;            // 运行期增益（SetChannelGain）

        // 样本级补偿（对齐 MediaX FrameProductionDiag::accumulatedCompensationUs / pendingProductionDrop）
        // 限速 COMP_FORCE_RATE_LIMIT_US = 5000us/s：每秒最多补 / 丢 5ms 样本，避免瞬间抽变音
        //
        // 累加语义采用 "delta 累加"（Review 后保守方案）：
        //   pendingCompUs += (当前帧 drift - 上一帧 drift)
        //   PI 稳态下 driftUs 近似常数，delta≈0，不会把 PI 已经维持住的稳态偏差再累一遍；
        //   ForceAlign / BURST 把时间戳一次性"跳齐"后，driftUs 的瞬时跳变不代表"新增偏差"，
        //   由 Push 侧检测跳变并刷新 prevDriftUs 跳过累加，避免双重补偿。
        int64_t pendingCompUs = 0;        // 本路 drift 累加器（正=本路慢，待补；负=本路快，待丢）
        int64_t prevDriftUs = 0;          // 上一帧 driftUs 快照（用于 delta 累加）
        bool    driftBaselineSet = false; // 首帧时 drift 不是"新增"，不参与 delta 计算
        int64_t lastCompUs = 0;           // 上一次做补偿的系统时间
        int64_t compensationUs = 0;       // 累计实际补偿量（统计用）
        int64_t productionDropUs = 0;     // 累计实际丢弃量（统计用）
    };

    Channel* Find(int id);
    const Channel* Find(int id) const;

    // Push 来自采集线程、Mix 来自混流线程，必须串行化
    mutable std::mutex m_mutex;

    int m_outSampleRate = 0;
    int m_outChannels = 0;
    int64_t m_outputPtsUs = 0;
    std::vector<Channel> m_channels;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_AUDIO_MIXER_H
