// 音频链路的总线节点
//
//   asrc.*       AudioSourceNode  —— 一路音频源（原生采集源 / 外部注入），capture 组，受控输出
//   audio_mixer  AudioMixerNode   —— N 路混成一路（输入端口数不限，端口号即混流通道号），
//                                    自带混音时钟线程，产出的 PCM 交给下游（audio_encoder），
//                                    并推进音频主时钟（BusContext 注入的 core::AudioClock）
//
//   asrc.mic ─┐
//   asrc.spk ─┼─> audio_mixer ──> audio_encoder ──> muxer
//   asrc.ext.N┘
//
// 运行期增删音频源 = 事务里 Register + Link(…, "audio_mixer", kAnyPort) / Unregister：
// 混音器在 OnInputAttached / OnInputDetached 里建 / 拆对应通道，端口由总线分配，删后再加不会撞号。
#ifndef TGCAPTURE_NODES_AUDIO_NODES_H
#define TGCAPTURE_NODES_AUDIO_NODES_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture/Source.h"
#include "core/AudioMixer.h"
#include "core/Bus.h"

namespace TGCapture::nodes
{

// ------------------------------------------------------------------ asrc.*
class AudioSourceNode : public core::BusNode
{
public:
    // 原生采集源（不拥有；调用方保证源的生命周期覆盖本节点在总线上的时间）
    AudioSourceNode(std::string name, capture::AudioSourceBase* source);
    // 外部注入源：宿主线程经 Push() 推帧
    AudioSourceNode(std::string name, AudioCapSourceType type);
    ~AudioSourceNode() override;

    [[nodiscard]] size_t InputPorts() const override { return 0; }
    bool Submit(core::FramePtr) override { return false; }   // 没有输入
    [[nodiscard]] size_t Pending() const override { return 0; }

    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Capture; }
    [[nodiscard]] bool IsActive() const override { return m_enabled.load(); }
    [[nodiscard]] bool GatedOutput() const override { return true; }

    // Prepare 时由门面设置：是否启用（纯视频模式下不启用）、原生源打开的格式
    void SetEnabled(bool enabled) noexcept { m_enabled.store(enabled); }
    void SetOpenFormat(int sampleRate, int channels) noexcept
    {
        m_sampleRate = sampleRate;
        m_channels = channels;
    }

    // 原生源：Open（失败按"降级而非失败"处理：返回 0，该路按静音处理）
    int NodeConfigure() override;
    bool NodeStart() override;
    void NodeStop() override;
    void NodeClose() override;

    // 外部注入：推一帧给下游。节点停止（关闸）后返回 false
    bool Push(core::FramePtr frame);

    // 解除与原生源的回调绑定（源停止后、节点或源销毁前调用）
    void Detach();

    [[nodiscard]] bool IsExternal() const noexcept { return m_source == nullptr; }
    [[nodiscard]] capture::AudioSourceBase* Source() const noexcept { return m_source; }
    [[nodiscard]] AudioCapSourceType Type() const noexcept { return m_type; }
    [[nodiscard]] uint64_t Emitted() const noexcept { return m_emitted.load(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    void OnSourceFrame(core::FramePtr frame);

    capture::AudioSourceBase* m_source = nullptr;
    AudioCapSourceType m_type = TypeOfANone;
    std::atomic_bool m_enabled{true};
    int m_sampleRate = 44100;
    int m_channels = 2;
    std::atomic<uint64_t> m_emitted{0};
};

// ------------------------------------------------------------------ audio_mixer
class AudioMixerNode : public core::BusNode
{
public:
    struct Config
    {
        int sampleRate = 44100;
        int channels = 2;
        int frameSize = 1024;                // 每帧样本数（每声道），取编码器的 frame_size
        int64_t clockCompensationUs = 0;     // 音频主时钟的固定补偿（VideoEncoderParams::audioCompensationUs）
    };

    AudioMixerNode();
    ~AudioMixerNode() override;

    [[nodiscard]] size_t InputPorts() const override { return core::kDynamicPorts; }
    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Capture; }
    [[nodiscard]] bool IsActive() const override { return m_enabled.load(); }

    bool Submit(core::FramePtr frame) override { return SubmitInput(0, std::move(frame)); }
    bool SubmitInput(size_t port, core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override { return 0; }

    void OnInputAttached(size_t port, const std::string& from) override;
    void OnInputDetached(size_t port, const std::string& from) override;

    void SetConfig(const Config& config);
    void SetEnabled(bool enabled) noexcept { m_enabled.store(enabled); }

    // Reset + 设输出格式 + 为当前已接入的输入预建通道（Prepare 时调用）
    int NodeConfigure() override;
    // 起混音时钟线程
    bool NodeStart() override;
    // 停线程 → 复位音频主时钟 → 打印汇总 → Reset 混流器
    void NodeStop() override;

    // 每输出一帧回调一次（混音线程内，参数为墙钟 us）：断流看护据此判断音频是否还在出
    void SetOutputObserver(std::function<void(int64_t)> fn);
    // 每 10s 回调一次（混音线程内）：门面打印心跳
    void SetHeartbeat(std::function<void()> fn);

    [[nodiscard]] core::AudioMixer& Mixer() noexcept { return m_mixer; }
    [[nodiscard]] const core::AudioMixer& Mixer() const noexcept { return m_mixer; }
    [[nodiscard]] bool IsMixing() const noexcept { return m_thread.joinable(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    void MixLoop(std::stop_token token);
    // 首帧到达时为该端口建通道（运行期新接入 / 外部注入）。失败返回 false
    bool EnsureChannel(size_t port, const core::Frame& frame);

    core::AudioMixer m_mixer;
    std::atomic_bool m_enabled{true};
    std::atomic_bool m_running{false};

    mutable std::mutex m_mutex;          // 保护下面三项
    Config m_config;
    std::map<size_t, std::string> m_inputs;   // 已接入的端口 -> 上游名
    std::map<size_t, bool> m_created;         // 端口 -> 混流通道是否已建

    std::function<void(int64_t)> m_onOutput;
    std::function<void()> m_onHeartbeat;
    std::jthread m_thread;   // 最后声明：析构时最先 join
};

}  // namespace TGCapture::nodes

#endif  // TGCAPTURE_NODES_AUDIO_NODES_H
