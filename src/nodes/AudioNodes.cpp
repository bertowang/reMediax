#include "nodes/AudioNodes.h"

#include <chrono>
#include <cstdio>

#include "core/AVSyncClock.h"
#include "core/Clock.h"
#include "core/Logger.h"

namespace TGCapture::nodes
{

// ================================================================== AudioSourceNode
AudioSourceNode::AudioSourceNode(std::string name, capture::AudioSourceBase* source)
    : core::BusNode(std::move(name)), m_source(source),
      m_type(source ? source->GetAudioCapSourceType() : TypeOfANone)
{
    if (m_source) {
        m_source->SetEmitCallback([this](core::FramePtr frame) { OnSourceFrame(std::move(frame)); });
    }
}

AudioSourceNode::AudioSourceNode(std::string name, AudioCapSourceType type)
    : core::BusNode(std::move(name)), m_type(type)
{
}

AudioSourceNode::~AudioSourceNode() = default;

void AudioSourceNode::Detach()
{
    // 源由宿主 / 门面持有，可能比节点活得久：解除回调，避免回调落到已销毁的节点上。
    // 只能在源已停止时调用（StopCapture 之后没有回调在飞）
    if (m_source) {
        m_source->SetEmitCallback(nullptr);
    }
}

int AudioSourceNode::NodeConfigure()
{
    if (!m_source || !m_enabled.load()) {
        return 0;
    }
    if (m_source->Open(m_sampleRate, m_channels) < 0) {
        // 音频按"降级而非失败"处理（少一路声音仍可开播），但不能就这么算了：
        // 开播后若混流器整体没有音频"输出"，断流看护会上报 TGErr_AudioCaptureStalled。
        // 注意：部分平台的采集源在打开失败后仍会注入静音帧，此时混流照常出帧，
        // 看护探测不到——这种"有声道但全静音"的情况只能靠日志排查，无法自动上报。
        TG_LOGW("AudioSourceNode[%s]: 音频源 type=%d 打开失败，该路按静音处理", Name().c_str(),
                static_cast<int>(m_type));
    }
    return 0;
}

bool AudioSourceNode::NodeStart()
{
    if (m_source) {
        m_source->StartCapture();
    }
    return true;
}

void AudioSourceNode::NodeStop()
{
    // 不按 enabled 过滤：Prepare 中途失败时它可能还是上一轮的值，
    // 而已经 Open 过的音频源无论如何都得停。对没启动的源是无副作用的空操作。
    if (m_source) {
        m_source->StopCapture();
    }
}

void AudioSourceNode::NodeClose()
{
    // 采集源的设备资源最后放：此时采集线程已 join、在飞回调已退出，Close 不会与产帧并发
    if (m_source) {
        m_source->Close();
    }
}

bool AudioSourceNode::Push(core::FramePtr frame)
{
    if (!frame) {
        return false;
    }
    m_emitted.fetch_add(1, std::memory_order_relaxed);
    return Emit(0, std::move(frame));
}

void AudioSourceNode::OnSourceFrame(core::FramePtr frame)
{
    if (!frame) {
        return;
    }
    m_emitted.fetch_add(1, std::memory_order_relaxed);
    Emit(0, std::move(frame));
}

std::string AudioSourceNode::DumpState() const
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "type=%d %s emitted=%llu", static_cast<int>(m_type),
                  m_source ? "native" : "external", static_cast<unsigned long long>(m_emitted.load()));
    return buf;
}

// ================================================================== AudioMixerNode
AudioMixerNode::AudioMixerNode() : core::BusNode("audio_mixer") {}

AudioMixerNode::~AudioMixerNode()
{
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

void AudioMixerNode::SetConfig(const Config& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    if (m_config.frameSize <= 0) {
        m_config.frameSize = 1024;
    }
}

void AudioMixerNode::SetOutputObserver(std::function<void(int64_t)> fn)
{
    m_onOutput = std::move(fn);
}

void AudioMixerNode::SetHeartbeat(std::function<void()> fn)
{
    m_onHeartbeat = std::move(fn);
}

void AudioMixerNode::OnInputAttached(size_t port, const std::string& from)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inputs[port] = from;
    m_created[port] = false;   // 通道在该端口首帧到达时按其格式建（外部注入的格式事先不知道）
}

std::optional<size_t> AudioMixerNode::PortOf(const std::string& upstream) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [port, name] : m_inputs) {
        if (name == upstream) {
            return port;
        }
    }
    return std::nullopt;
}

void AudioMixerNode::SetInputGain(size_t port, float gain)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gains[port] = gain;
    auto it = m_created.find(port);
    if (it != m_created.end() && it->second) {
        m_mixer.SetChannelGain(static_cast<int>(port), gain);
    }
}

void AudioMixerNode::FadeOutInput(size_t port)
{
    if (!m_running.load()) {
        return;
    }
    m_mixer.FadeOutChannel(static_cast<int>(port));
}

bool AudioMixerNode::InputFaded(size_t port) const
{
    return !m_running.load() || m_mixer.IsChannelFaded(static_cast<int>(port));
}

core::AudioMixer::Level AudioMixerNode::InputLevel(size_t port) const
{
    return m_mixer.ChannelLevel(static_cast<int>(port));
}

void AudioMixerNode::OnInputDetached(size_t port, const std::string& from)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inputs.erase(port);
    m_gains.erase(port);
    auto it = m_created.find(port);
    if (it != m_created.end()) {
        if (it->second) {
            m_mixer.RemoveChannel(static_cast<int>(port));
            TG_LOGI("AudioMixerNode: 输入断开 port=%zu from=%s，通道已移除", port, from.c_str());
        }
        m_created.erase(it);
    }
}

int AudioMixerNode::NodeConfigure()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mixer.Reset();
    m_mixer.SetOutputFormat(m_config.sampleRate, m_config.channels);
    // 已接入的输入（原生源，格式即配置格式）预建通道：与原先 Prepare 里按源顺序建通道的行为一致
    const int64_t nowUs = core::MonotonicClock::NowUs();
    for (auto& [port, created] : m_created) {
        created = m_mixer.AddChannel(static_cast<int>(port), m_config.sampleRate, m_config.channels,
                                     nowUs) == 0;
        ApplyGainLocked(port);
    }
    return 0;
}

void AudioMixerNode::ApplyGainLocked(size_t port)
{
    auto g = m_gains.find(port);
    if (g != m_gains.end()) {
        m_mixer.SetChannelGain(static_cast<int>(port), g->second);
    }
}

bool AudioMixerNode::EnsureChannel(size_t port, const core::Frame& frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_created.find(port);
    if (it == m_created.end()) {
        return false;   // 未接入的端口（不应发生）
    }
    if (it->second) {
        return true;
    }
    // 输出格式始终是编码器的配置格式（编码器不做采样率转换）；各路输入由通道自己的 swr 归一
    if (m_mixer.AddChannel(static_cast<int>(port), frame.sampleRate, frame.channels,
                           core::MonotonicClock::NowUs()) != 0) {
        TG_LOGE("AudioMixerNode: 为输入 port=%zu（%s）创建混流通道失败", port,
                m_inputs.count(port) ? m_inputs[port].c_str() : "?");
        ReportError(TGErr_Internal, "add audio mixer channel failed");
        return false;
    }
    it->second = true;
    ApplyGainLocked(port);
    TG_LOGI("AudioMixerNode: 输入 port=%zu（%s）占用混流通道（%dHz %dch）", port,
            m_inputs.count(port) ? m_inputs[port].c_str() : "?", frame.sampleRate, frame.channels);
    return true;
}

bool AudioMixerNode::SubmitInput(size_t port, core::FramePtr frame)
{
    if (!frame || !m_running.load() || !m_enabled.load()) {
        return false;
    }
    if (!frame->audioData || frame->audioBytes == 0 || frame->channels <= 0) {
        return false;
    }
    if (!EnsureChannel(port, *frame)) {
        return false;
    }
    const int frames = static_cast<int>(frame->audioBytes /
                                        (sizeof(int16_t) * static_cast<size_t>(frame->channels)));
    m_mixer.Push(static_cast<int>(port), reinterpret_cast<const int16_t*>(frame->audioData.get()),
                 frames, frame->captureUs);
    return true;
}

bool AudioMixerNode::NodeStart()
{
    if (m_thread.joinable()) {
        return true;
    }
    m_running.store(true);
    TG_LOGI("AudioMixerNode: 启用音频混流（%zu 路，输出 %dHz %dch）", m_mixer.ChannelCount(),
            m_mixer.OutputSampleRate(), m_mixer.OutputChannels());
    m_thread = std::jthread([this](std::stop_token st) { MixLoop(st); });
    return true;
}

void AudioMixerNode::NodeStop()
{
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
    // 混流器与音频主时钟必须一起复位：AudioClock::AdvanceTo 是单向的（只在前进方向生效），
    // 若只把 mixer 的输出时间轴归零、而音频主时钟还停在上一轮的末值，
    // 新一轮的 AdvanceTo 会被判成"回退"全部忽略，视频 PTS 就会从上一轮时长起算。
    if (auto ctx = Context()) {
        if (auto clock = ctx->Get<core::AudioClock>()) {
            clock->Reset();
            std::lock_guard<std::mutex> lock(m_mutex);
            clock->SetCompensationUs(m_config.clockCompensationUs);
        }
    }
    const auto ms = m_mixer.Stats();
    TG_LOGI("TGCapturer: 混流汇总 out=%llu wait=%llu reanchor=%llu/%lldms",
            static_cast<unsigned long long>(ms.outputFrames),
            static_cast<unsigned long long>(ms.waitTimeouts),
            static_cast<unsigned long long>(ms.reanchors), static_cast<long long>(ms.reanchorUs / 1000));
    auto diags = m_mixer.AllDiagnostics();
    for (size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        TG_LOGI("TGCapturer: 混流通道 %zu under=%llu comp=%lldms drop=%lldms ovf=%lldms "
                "drift=%lldus/5s tsBad=%llu fallback=%d jump=%llu",
                i, static_cast<unsigned long long>(d.underrun),
                static_cast<long long>(d.compensationUs / 1000),
                static_cast<long long>(d.productionDropUs / 1000),
                static_cast<long long>(d.overflowDropUs / 1000),
                static_cast<long long>(d.residualDriftUs),
                static_cast<unsigned long long>(d.tsAnomalies), d.tsFallback ? 1 : 0,
                static_cast<unsigned long long>(d.clock.jumpCount));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mixer.Reset();
    for (auto& [_, created] : m_created) {
        created = false;
    }
}

void AudioMixerNode::MixLoop(std::stop_token token)
{
    int frameSize = 1024;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        frameSize = m_config.frameSize;
    }
    std::shared_ptr<core::AudioClock> clock;
    if (auto ctx = Context()) {
        clock = ctx->Get<core::AudioClock>();
    }
    // 心跳：每 10 秒打一条累计状态，长压测时能在时间轴上看到 drift / avDiff 的分布，
    // 而不是只在 Stop 时才有一次最终聚合
    const int64_t kHeartbeatUs = 10 * 1000000LL;
    int64_t lastBeatUs = core::MonotonicClock::NowUs();
    int64_t lastOutputUs = lastBeatUs;
    while (!token.stop_requested()) {
        std::shared_ptr<uint8_t> data;
        int64_t ptsUs = 0;
        if (!m_mixer.Mix(frameSize, data, ptsUs)) {
            // 混流按输出时钟出帧，轮询时取不到数据是常态；只有持续 500ms 没有任何输出才提示
            // （全部通道断流 / 未推数据，音频时间轴停住）
            if (core::MonotonicClock::NowUs() - lastOutputUs > 500000) {
                TG_LOGW_THROTTLED(3000000,
                                  "TGCapturer: 混流超过 500ms 无输出（当前通道数 %zu，全部断流或未推数据）",
                                  m_mixer.ChannelCount());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        lastOutputUs = core::MonotonicClock::NowUs();
        if (m_onOutput) {
            m_onOutput(lastOutputUs);
        }

        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (nowUs - lastBeatUs >= kHeartbeatUs) {
            lastBeatUs = nowUs;
            if (m_onHeartbeat) {
                m_onHeartbeat();
            }
        }

        const int channels = m_mixer.OutputChannels();
        const int sampleRate = m_mixer.OutputSampleRate();
        const size_t bytes =
            static_cast<size_t>(frameSize) * static_cast<size_t>(channels) * sizeof(int16_t);

        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Audio;
        frame->sampleRate = sampleRate;
        frame->channels = channels;
        frame->sampleFmt = AV_SAMPLE_FMT_S16;
        frame->planar = false;
        frame->nbSamples = frameSize;
        frame->audioData = data;
        frame->audioBytes = bytes;
        frame->ptsUs = ptsUs;
        frame->captureUs = core::MonotonicClock::NowUs();

        // 音频主时钟跟着混流输出走（视频 PTS 由它反推）
        if (clock && sampleRate > 0) {
            clock->AdvanceTo(ptsUs + static_cast<int64_t>(frameSize) * 1000000LL / sampleRate);
        }
        Emit(0, std::move(frame));
    }
}

std::string AudioMixerNode::DumpState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "inputs=%zu channels=%zu out=%dHz/%dch mixing=%d",
                  m_inputs.size(), m_mixer.ChannelCount(), m_mixer.OutputSampleRate(),
                  m_mixer.OutputChannels(), m_thread.joinable() ? 1 : 0);
    return buf;
}

}  // namespace TGCapture::nodes
