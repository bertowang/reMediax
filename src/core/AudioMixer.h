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
// 出帧条件（输出时钟）：
//
//   所有"活跃通道"（kChannelIdleUs 内推过数据）都凑够一帧才出帧；有通道迟到时最多等
//   kMixWaitFrames 帧（墙钟超时 或 就绪通道已积压 kMixWaitFrames 帧，二者任一），超时后
//   迟到通道本帧按静音处理、已有的残余数据保留到下一帧（不丢样本）。
//   旧实现"任一路凑够即出帧 + 欠载路清空"在多路相位错开时会让每路各自触发出帧，
//   输出帧数≈各路帧数之和（60s 墙钟产出 114s 音频），且真实内容中间夹静音。
//
// 输出时间轴：
//
//   输出 PTS = 输出样本数线性累加（PTS 与实际样本数严格一致，编码器/播放器不会看到时间轴
//   与内容错位）。各路设备时钟相对墙钟的漂移在样本域纠正：swr_set_compensation 平滑伸缩
//   （每秒最多 5ms，≈0.5% 变速，不可闻），不再改写 PTS、也不再尾部复制/截断样本（会出咔声）。
//   全部通道断流超过 kReanchorGapUs 后恢复时，按墙钟把输出 PTS 前移到真实时间（只前移不回退），
//   保证视频（由音频时钟反推）不会因为音频断流而整体滞后。
//
// AudioChannelClock 的 PI/BURST 结果保留为诊断统计（drift/jump），不再写入输出 PTS。
//
// 运行期增删通道（输出流不中断）：
//   - 输出开始后新加的通道自动淡入 kFadeUs（20ms），避免"啪"的一声切入
//   - 移除前先 FadeOutChannel：用该路接下来的 20ms 数据淡出到 0，IsChannelFaded 为真后再 RemoveChannel
//   - 输出开始后若没有任何"已出过声"的通道（全部移除 / 新加的还没推数据），按墙钟输出静音帧，
//     音频时间轴（以及由它反推的视频 PTS）照常推进。已推过数据的通道断流仍按原逻辑不出帧（断流看护据此上报）
//
// 电平：每次出帧时按各路实际混入的样本（含增益与淡入淡出）统计峰值 / RMS，峰值带衰减保持。
#ifndef TGCAPTURE_CORE_AUDIO_MIXER_H
#define TGCAPTURE_CORE_AUDIO_MIXER_H

#include <atomic>
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

    [[nodiscard]] int OutputSampleRate() const { return m_outSampleRate.load(); }
    [[nodiscard]] int OutputChannels() const { return m_outChannels.load(); }
    [[nodiscard]] int64_t OutputPtsUs() const { return m_outputPtsUs; }
    [[nodiscard]] size_t BufferedFrames(int id) const;
    // 加锁：m_channels 会被 AddChannel 扩容，而本接口在运行期由混流线程 / 采集线程读
    [[nodiscard]] size_t ChannelCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_channels.size();
    }

    // 各路校正统计（诊断用）
    [[nodiscard]] AudioChannelClock::Stats ChannelStats(int id) const;

    // 诊断信息：PI/BURST 统计 + 本路 underrun + 样本级补偿（对齐 MediaX FrameProductionDiag）
    struct ChannelDiag
    {
        AudioChannelClock::Stats clock{};
        uint64_t underrun = 0;              // 活跃但迟到、本帧按静音处理的次数
        int64_t compensationUs = 0;         // 累计补偿（本路慢，swr 拉伸多产的样本时长）
        int64_t productionDropUs = 0;       // 累计丢弃（本路快，swr 压缩少产的样本时长）
        int64_t overflowDropUs = 0;         // 缓冲超上限丢弃的最旧样本时长
        int64_t residualDriftUs = 0;        // 最近一个漂移窗口（5s）测得的原始漂移
        uint64_t tsAnomalies = 0;           // 时间戳异常次数（回退 / 速率偏离 > 5%）
        bool tsFallback = false;            // 是否已因时间戳异常改用墙钟
    };
    [[nodiscard]] ChannelDiag ChannelDiagnostic(int id) const;
    // 便捷接口：按 index 顺序取全部通道诊断，供 TGCapturer::GetStatsInfo 序列化
    [[nodiscard]] std::vector<ChannelDiag> AllDiagnostics() const;

    struct MixerStats
    {
        uint64_t outputFrames = 0;   // 已输出帧数
        uint64_t waitTimeouts = 0;   // 等待迟到通道超时后出帧的次数
        uint64_t reanchors = 0;      // 全路断流恢复后按墙钟前移输出 PTS 的次数
        int64_t reanchorUs = 0;      // 累计前移量
    };
    [[nodiscard]] MixerStats Stats() const;

    // 运行期通道增益（线性，0.0 ~ 4.0；clamp 由 Mixer 内部完成，不停流不重建管线）
    void SetChannelGain(int id, float gain);

    // 淡入淡出时长
    static constexpr int64_t kFadeUs = 20000;
    // 开始淡出（之后该路的数据在 durUs 内线性衰减到 0，此后不再混入）
    void FadeOutChannel(int id, int64_t durUs = kFadeUs);
    // 淡出完成（或该路已不存在 / 输出尚未开始）
    [[nodiscard]] bool IsChannelFaded(int id) const;

    // 电平（线性，0 ~ 1，相对 S16 满幅）
    struct Level
    {
        float peak = 0.0f;
        float rms = 0.0f;
    };
    [[nodiscard]] Level ChannelLevel(int id) const;
    [[nodiscard]] Level MasterLevel() const;

private:
    struct Channel
    {
        int id = -1;
        int sampleRate = 0;
        int channels = 0;
        bool active = false;
        AudioChannelClock clock;
        std::deque<int16_t> pcm;      // 交错 S16（已归一到输出格式）
        SwrContext* swr = nullptr;    // 归一到输出格式 + 漂移补偿（始终创建，强制走重采样器）
        uint64_t underrun = 0;        // 活跃但迟到的次数
        float gain = 1.0f;            // 运行期增益（SetChannelGain）
        int64_t lastPushWallUs = 0;   // 最近一次 Push 的墙钟（判定通道是否活跃）

        // 淡入 / 淡出（单位：输出样本帧）。淡出开始后 removing=true，淡完 faded=true（不再混入）
        int64_t fadeInTotal = 0;
        int64_t fadeInPos = 0;
        bool removing = false;
        bool faded = false;
        int64_t fadeOutTotal = 0;
        int64_t fadeOutPos = 0;
        Level level{};

        // 漂移测量：offset_i = (ts_i - ts_origin) - 已推样本时长_i，按 5s 窗口取平均，
        // 相邻窗口平均值之差即本窗口的速率漂移（抖动被平均掉，不随时间累积）。
        // 时间戳断档 / 通道停推后恢复时重置原点（断流不是速率漂移，不能补）。
        bool    winValid = false;
        int64_t originTsUs = 0;
        int64_t cumSamples = 0;       // 自原点起已推样本数（输入采样率）
        int64_t winStartTsUs = 0;
        int64_t winStartWallUs = 0;
        int64_t winOffsetSum = 0;
        int64_t winCount = 0;
        bool    havePrevAvg = false;
        int64_t prevAvgOffsetUs = 0;
        int64_t lastTsUs = 0;         // 上一帧时间戳
        int64_t lastFrameDurUs = 0;   // 上一帧时长
        int64_t residualDriftUs = 0;  // 最近一个窗口测得的漂移（诊断）

        // 时间戳合理性：回退或推进速率偏离墙钟 > 5% 时判为不可信，本路此后改用墙钟
        bool     tsUntrusted = false;
        uint64_t tsAnomalies = 0;

        // 样本域补偿：pendingCompUs > 0 本路慢（样本不够）需拉伸；< 0 本路快需压缩。
        // 通过 swr_set_compensation 在 1s 内平滑完成，每次最多 5ms。
        int64_t pendingCompUs = 0;
        int64_t lastCompWallUs = 0;       // 上一次下发补偿的墙钟
        int64_t compensationUs = 0;       // 累计拉伸量（统计用）
        int64_t productionDropUs = 0;     // 累计压缩量（统计用）
        int64_t overflowDropUs = 0;       // 缓冲超上限丢弃量（统计用）
    };

    Channel* Find(int id);
    const Channel* Find(int id) const;
    // 漂移测量 + 样本域补偿；返回本帧实际采用的时间戳（调用方时间戳 或 墙钟兜底）
    int64_t MeasureDriftAndCompensate(Channel& ch, int frames, int64_t captureUs, int64_t wallUs,
                                      bool callerTs, bool resumed);
    static void FallbackToWall(Channel& ch, const char* reason, int64_t valueUs);
    static void ResetChannelRuntime(Channel& ch);

    // Push 来自采集线程、Mix 来自混流线程，必须串行化
    mutable std::mutex m_mutex;

    // 原子：首路外灌音频到达时由采集线程 SetOutputFormat 写，混流线程同时在读。
    // 写侧另外持 m_mutex，保证 (采样率, 声道数) 这一对对 Mix 而言是整体生效的。
    std::atomic<int> m_outSampleRate{0};
    std::atomic<int> m_outChannels{0};
    int64_t m_outputPtsUs = 0;        // 下一帧输出 PTS = base + samples / SR
    int64_t m_outputPtsBaseUs = 0;
    int64_t m_outputSamples = 0;      // 自 base 起已输出样本数（按样本累加，避免逐帧取整误差累积）
    std::vector<Channel> m_channels;

    // 输出时钟
    bool m_outputStarted = false;
    int64_t m_waitStartWallUs = 0;    // 开始等待迟到通道的墙钟（0=未在等待）
    int64_t m_anchorWallUs = 0;       // 墙钟锚点：首帧输出时的墙钟，对应 PTS = m_anchorPtsUs
    int64_t m_anchorPtsUs = 0;
    int64_t m_lagWinStartWallUs = 0;  // 最小 lag 统计窗口
    int64_t m_lagWinMinUs = 0;
    MixerStats m_stats{};
    Level m_masterLevel{};

    void ShiftOutputTimeline(int64_t shiftUs);
    // 把一路的 n 个样本（交错）按增益 / 淡入淡出加到 acc，并统计本帧电平（按 frameSamples 个样本归一）
    void Accumulate(Channel& ch, std::vector<int32_t>& acc, size_t n, size_t frameSamples);

public:
    AudioMixer() = default;
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_AUDIO_MIXER_H
