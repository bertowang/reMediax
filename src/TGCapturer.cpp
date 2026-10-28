#include "TGCapturer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "core/Logger.h"

#ifdef _WIN32
#include "platform/windows/WinAudioCapture.h"
#include "platform/windows/WinScreenCapture.h"
#elif defined(__ANDROID__)
#include "platform/android/AndroidAudioCapture.h"
#include "platform/android/AndroidScreenCapture.h"
#elif defined(__APPLE__)
#include "platform/ios/IOSAudioCapture.h"
#include "platform/ios/IOSScreenCapture.h"
#endif

// 有原生采集实现的平台。Linux 等平台没有（屏幕需 X11/PipeWire、音频需 PulseAudio/ALSA）：
// 采集接口一律返回不支持，但编码 / 封装 / 外灌帧不依赖它，故必须能编译通过。
// 没有这个宏时，下面那些 `#else return` 分支之后仍会引用未声明的 source，编译不过。
#if defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__)
#define TG_HAS_NATIVE_CAPTURE 1
#else
#define TG_HAS_NATIVE_CAPTURE 0
#endif

namespace TGCapture
{

TGCapturer::TGCapturer(GameID gameId) : m_gameId(gameId)
{
    m_sync = std::make_shared<core::AVSync>();
    m_audioClock = std::make_shared<core::AudioClock>();
    m_videoPtsGen = std::make_shared<core::VideoPtsGenerator>();
    m_videoOnlyTimeline = std::make_shared<core::VideoOnlyTimeline>();
    m_mixer = std::make_unique<core::AudioMixer>();
    m_videoEncoder = std::make_shared<codec::VideoEncoder>();
    m_audioEncoder = std::make_shared<codec::AudioEncoder>();
    m_muxer = std::make_shared<Muxer>();
    m_pauseFrame = std::make_shared<core::PauseFrame>();
    m_watermark = std::make_shared<core::Watermark>();

    m_videoEncoder->SetSyncClock(m_sync);
    m_audioEncoder->SetSyncClock(m_sync);

    // 总线拓扑：采集 → video_encoder / audio_encoder → muxer
    // 数据、命令、事件都走这一条总线（见 core/Bus.h）；拓扑只在这里建立，运行期只读
    m_bus.Register("video_encoder", m_videoEncoder);
    m_bus.Register("audio_encoder", m_audioEncoder);
    m_bus.Register("muxer", m_muxer);
    const bool linked = m_bus.Link("video_encoder", "muxer") && m_bus.Link("audio_encoder", "muxer");
    if (!linked) {
        TG_LOGE("TGCapturer: 总线拓扑建立失败（video/audio_encoder → muxer）");
    }
    m_videoIn = m_bus.Get("video_encoder");
    m_audioIn = m_bus.Get("audio_encoder");

    // 事件：错误 / 通知 / 生命周期统一从总线分发，OnBusEvent 里做联动并转给宿主回调
    m_bus.Subscribe([this](const core::BusEvent& event) { OnBusEvent(event); });
}

TGCapturer::~TGCapturer()
{
    Stop();
    ReleaseAll();
    // 先停事件分发再销毁成员：订阅者捕获了 this。
    // 若本析构就发生在某个宿主回调里（分发线程内），Shutdown 会分离线程而不是 join 自己
    m_bus.Events().Shutdown();
}

// ------------------------------------------------------------------ 采集源
bool TGCapturer::IsSupported(CaptureFeature feature) const
{
#if defined(_WIN32)
    (void)feature;
    return true;    // 屏幕 / 窗口 / 麦克风 / 扬声器 全部支持
#elif defined(__ANDROID__)
    // Android 没有"窗口"概念；系统内录（扬声器）需 Android 10(API 29)+ 且持有 MediaProjection
    return feature != CaptureFeature::Window;
#elif defined(__APPLE__)
    // iOS：ReplayKit 只能录本应用（系统级需 Broadcast Upload Extension）；无窗口概念、无系统内录
    return feature == CaptureFeature::Screen || feature == CaptureFeature::Microphone;
#else
    (void)feature;
    return false;
#endif
}

int TGCapturer::EnableMicrophoneCapture(bool usingGVoice)
{
    if (usingGVoice) {
        // 明确不再接 GVoice：一律走系统麦克风（WASAPI / AudioRecord / AVAudioSession）
        TG_LOGW("TGCapturer: GVoice 已停用，改用系统麦克风采集");
    }
    if (!IsSupported(CaptureFeature::Microphone)) {
        TG_LOGE("TGCapturer: 当前平台不支持麦克风采集");
        return TGErr_NotSupported;
    }
#if defined(_WIN32)
    auto source = std::make_shared<platform::WinAudioCapture>(TypeOfSelfMicphone, false);
#elif defined(__ANDROID__)
    auto source = std::make_shared<platform::AndroidAudioCapture>(TypeOfSelfMicphone, false);
#elif defined(__APPLE__)
    auto source = std::make_shared<platform::IOSAudioCapture>(TypeOfSelfMicphone);
#endif

#if TG_HAS_NATIVE_CAPTURE
    m_ownedAudioSources.push_back(source);
    return AddAudioSource(source.get());
#else
    TG_LOGW("TGCapturer: 当前平台无麦克风采集实现（外部注入请用 PushExternalAudioFrame）");
    return TGErr_NotSupported;
#endif
}

int TGCapturer::EnableSpeakerCapture()
{
    if (!IsSupported(CaptureFeature::Speaker)) {
        TG_LOGE("TGCapturer: 当前平台不支持扬声器/系统内录");
        return TGErr_NotSupported;
    }
#if defined(_WIN32)
    auto source = std::make_shared<platform::WinAudioCapture>(TypeOfSpeaker, true);  // WASAPI loopback
#elif defined(__ANDROID__)
    auto source = std::make_shared<platform::AndroidAudioCapture>(TypeOfSpeaker, true);  // AudioPlaybackCapture
#endif

#if defined(_WIN32) || defined(__ANDROID__)
    m_ownedAudioSources.push_back(source);
    return AddAudioSource(source.get());
#else
    // iOS 没有系统内录：需宿主 App 加 Broadcast Upload Extension 后走 PushExternalAudioFrame
    TG_LOGW("TGCapturer: 当前平台无扬声器/系统内录实现");
    return TGErr_NotSupported;
#endif
}

IVideoSource* TGCapturer::CreateScreenCapture(int index, VideoCapMode mode, VideoScaleStrategy strategy)
{
    if (!IsSupported(CaptureFeature::Screen)) {
        TG_LOGE("TGCapturer: 当前平台不支持屏幕采集");
        return nullptr;
    }
#if defined(_WIN32)
    auto source = std::make_shared<platform::WinScreenCapture>(index, mode, strategy);
#elif defined(__ANDROID__)
    auto source = std::make_shared<platform::AndroidScreenCapture>(strategy);
    source->SetProjection(m_mediaProjection);
#elif defined(__APPLE__)
    auto source = std::make_shared<platform::IOSScreenCapture>(strategy);
#endif

#if TG_HAS_NATIVE_CAPTURE
    m_ownedVideoSources.push_back(source);
    m_videoSource = source.get();
    return source.get();
#else
    TG_LOGW("TGCapturer: 当前平台无屏幕采集实现（外部注入请用 PushExternalVideoFrame）");
    return nullptr;
#endif
}

IVideoSource* TGCapturer::CreateWindowCapture(void* handle, VideoCapMode mode, VideoScaleStrategy strategy)
{
    if (!IsSupported(CaptureFeature::Window)) {
        TG_LOGE("TGCapturer: 窗口采集仅 Windows 支持（Android/iOS 没有窗口概念）");
        return nullptr;
    }
#if defined(_WIN32)
    auto source = std::make_shared<platform::WinScreenCapture>(static_cast<HWND>(handle), mode, strategy);
    m_ownedVideoSources.push_back(source);
    m_videoSource = source.get();
    return source.get();
#else
    (void)handle;
    (void)mode;
    (void)strategy;
    return nullptr;
#endif
}

IVideoSource* TGCapturer::CreateLocalFileCapture(int index, const std::string& filePath, bool loopPlay)
{
    TG_LOGW("TGCapturer: 本地文件采集暂未实现（开播链路不涉及）");
    return nullptr;
}

int TGCapturer::AddAudioSource(IAudioSource* source)
{
    if (!source) {
        TG_LOGE("TGCapturer: AddAudioSource 传入空指针");
        return TGErr_InvalidArgument;
    }
    auto* base = dynamic_cast<capture::AudioSourceBase*>(source);
    if (!base) {
        TG_LOGE("TGCapturer: AddAudioSource 的对象不是本 SDK 创建的音频源");
        return TGErr_InvalidArgument;
    }
    if (std::find(m_audioSources.begin(), m_audioSources.end(), base) != m_audioSources.end()) {
        return 0;   // 已添加
    }
    m_audioSources.push_back(base);
    if (!m_audioSource) {
        m_audioSource = base;
    }
    base->SetEmitCallback([this, base](core::FramePtr frame) { OnAudioFrameFrom(base, std::move(frame)); });
    TG_LOGI("TGCapturer: 音频源 +1（当前 %zu 路）", m_audioSources.size());
    return 0;
}

int TGCapturer::ChannelIdOf(capture::AudioSourceBase* source) const
{
    for (size_t i = 0; i < m_audioSources.size(); ++i) {
        if (m_audioSources[i] == source) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TGCapturer::OnAudioFrameFrom(capture::AudioSourceBase* source, core::FramePtr frame)
{
    if (!frame || !m_audioEnabled || !m_mixer || m_state.load() != State::Running) {
        return;
    }
    if (!frame->audioData || frame->audioBytes == 0 || frame->channels <= 0) {
        return;
    }
    // 统一进混流器：单路也同样走（这样单路也能吃到 PI/BURST 时钟校正）
    const int id = ChannelIdOf(source);
    if (id < 0) {
        return;
    }
    const int frames = static_cast<int>(frame->audioBytes / (sizeof(int16_t) * static_cast<size_t>(frame->channels)));
    m_mixer->Push(id, reinterpret_cast<const int16_t*>(frame->audioData.get()), frames,
                  frame->captureUs);
}

void TGCapturer::StartAudioMixer()
{
    if (!m_audioEnabled || m_mixThread.joinable()) {
        return;
    }
    TG_LOGI("TGCapturer: 启用音频混流（%zu 路，输出 %dHz %dch）",
            m_audioSources.size(), m_mixer->OutputSampleRate(), m_mixer->OutputChannels());
    m_mixThread = std::jthread([this](std::stop_token st) { MixLoop(st); });
}

void TGCapturer::StopAudioMixer()
{
    if (m_mixThread.joinable()) {
        m_mixThread.request_stop();
        m_mixThread.join();
    }
    // 混流器与音频主时钟必须一起复位：AudioClock::AdvanceTo 是单向的（只在前进方向生效），
    // 若只把 mixer 的输出时间轴归零、而 m_audioClock 还停在上一轮的末值，
    // 新一轮的 AdvanceTo 会被判成"回退"全部忽略，视频 PTS 就会从上一轮时长起算。
    if (m_audioClock) {
        m_audioClock->Reset();
        m_audioClock->SetCompensationUs(m_videoParams.audioCompensationUs);
    }
    if (m_mixer) {
        const auto ms = m_mixer->Stats();
        TG_LOGI("TGCapturer: 混流汇总 out=%llu wait=%llu reanchor=%llu/%lldms",
                static_cast<unsigned long long>(ms.outputFrames),
                static_cast<unsigned long long>(ms.waitTimeouts),
                static_cast<unsigned long long>(ms.reanchors),
                static_cast<long long>(ms.reanchorUs / 1000));
        auto diags = m_mixer->AllDiagnostics();
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
        m_mixer->Reset();
    }
}

void TGCapturer::MixLoop(std::stop_token token)
{
    // 心跳：每 10 秒打一条累计状态，长压测时能在时间轴上看到 drift / avDiff 的分布，
    // 而不是只在 Stop 时才有一次最终聚合
    const int64_t kHeartbeatUs = 10 * 1000000LL;
    int64_t lastBeatUs = core::MonotonicClock::NowUs();
    int64_t lastOutputUs = lastBeatUs;
    while (!token.stop_requested()) {
        std::shared_ptr<uint8_t> data;
        int64_t ptsUs = 0;
        if (!m_mixer->Mix(m_audioFrameSize, data, ptsUs)) {
            // 混流按输出时钟出帧，轮询时取不到数据是常态；只有持续 500ms 没有任何输出才提示
            // （全部通道断流 / 未推数据，音频时间轴停住）
            if (core::MonotonicClock::NowUs() - lastOutputUs > 500000) {
                TG_LOGW_THROTTLED(3000000, "TGCapturer: 混流超过 500ms 无输出（当前通道数 %zu，全部断流或未推数据）",
                                  m_mixer->ChannelCount());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        lastOutputUs = core::MonotonicClock::NowUs();
        m_audioWatch.OnActivity(lastOutputUs);

        // ---- 心跳：10s/次 I 级累计状态 ----
        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (nowUs - lastBeatUs >= kHeartbeatUs) {
            lastBeatUs = nowUs;
            // 1) 视频 PTS 生成器累计
            core::VideoPtsGenerator::Stats vs{};
            if (m_videoPtsGen) { vs = m_videoPtsGen->stats(); }
            // 2) 每路音频的 drift / PI / BURST / ForceAlign / 积压
            auto diags = m_mixer->AllDiagnostics();
            std::string per;
            per.reserve(128);
            for (size_t i = 0; i < diags.size(); ++i) {
                const auto& d = diags[i];
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              " ch%zu[drift=%lldus/5s jump=%llu under=%llu comp=%lldms drop=%lldms "
                              "ovf=%lldms tsBad=%llu%s buf=%zu]",
                              i,
                              static_cast<long long>(d.residualDriftUs),
                              static_cast<unsigned long long>(d.clock.jumpCount),
                              static_cast<unsigned long long>(d.underrun),
                              static_cast<long long>(d.compensationUs / 1000),
                              static_cast<long long>(d.productionDropUs / 1000),
                              static_cast<long long>(d.overflowDropUs / 1000),
                              static_cast<unsigned long long>(d.tsAnomalies),
                              d.tsFallback ? "(wall)" : "",
                              m_mixer->BufferedFrames(static_cast<int>(i)));
                per += buf;
            }
            const auto ms = m_mixer->Stats();
            TG_LOGI("TGCapturer: 心跳 avDiff=%lldms force=%llu adapt=%llu clamp=%llu "
                    "stall=%llu skip=%llu jump=%llu staleReset=%llu bpDrop=%llu "
                    "bus[pend=%zu evDrop=%llu] mix[out=%llu wait=%llu reanchor=%llu/%lldms]|%s",
                    static_cast<long long>(vs.avDiffUs / 1000),
                    static_cast<unsigned long long>(vs.forceSynced),
                    static_cast<unsigned long long>(vs.adapted),
                    static_cast<unsigned long long>(vs.clamped),
                    static_cast<unsigned long long>(vs.stalled),
                    static_cast<unsigned long long>(vs.skipped),
                    static_cast<unsigned long long>(vs.jumpWarns),
                    static_cast<unsigned long long>(vs.staleResets),
                    static_cast<unsigned long long>(m_bpDropped.load()),
                    m_bus.TotalPending(),
                    static_cast<unsigned long long>(m_bus.Events().Dropped()),
                    static_cast<unsigned long long>(ms.outputFrames),
                    static_cast<unsigned long long>(ms.waitTimeouts),
                    static_cast<unsigned long long>(ms.reanchors),
                    static_cast<long long>(ms.reanchorUs / 1000),
                    per.c_str());
        }

        const int channels = m_mixer->OutputChannels();
        const int sampleRate = m_mixer->OutputSampleRate();
        const size_t bytes = static_cast<size_t>(m_audioFrameSize) * static_cast<size_t>(channels) *
                             sizeof(int16_t);

        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Audio;
        frame->sampleRate = sampleRate;
        frame->channels = channels;
        frame->sampleFmt = AV_SAMPLE_FMT_S16;
        frame->planar = false;
        frame->nbSamples = m_audioFrameSize;
        frame->audioData = data;
        frame->audioBytes = bytes;
        frame->ptsUs = ptsUs;
        frame->captureUs = core::MonotonicClock::NowUs();

        // 音频主时钟跟着混流输出走（视频 PTS 由它反推）
        if (sampleRate > 0) {
            m_audioClock->AdvanceTo(ptsUs + static_cast<int64_t>(m_audioFrameSize) * 1000000LL /
                                                sampleRate);
        }
        m_audioIn->Submit(std::move(frame));
    }
}

int TGCapturer::AddVideoSource(IVideoSource* source)
{
    if (!source) {
        TG_LOGE("TGCapturer: AddVideoSource 传入空指针");
        return TGErr_InvalidArgument;
    }
    auto* base = dynamic_cast<capture::VideoSourceBase*>(source);
    if (!base) {
        TG_LOGE("TGCapturer: AddVideoSource 的对象不是本 SDK 创建的视频源");
        return TGErr_InvalidArgument;
    }
    m_videoSource = base;
    base->SetEmitCallback([this](core::FramePtr frame) { OnVideoFrame(std::move(frame)); });
    return 0;
}

// ------------------------------------------------------------------ 参数
// 三个 Setup 都在这里做"明显非法"的校验并当场拒绝：
// 坏参数若被记下来，故障会推迟到 Prepare 才以 OpenXxx 的形式冒出来，排障要多绕一圈。
// 拒绝时**不记录**该组参数，于是后续 Prepare 会返回 TGErr_NotConfigured —— 语义上就是"没配过"。
int TGCapturer::SetupAudioEncoder(const AudioEncoderParams& params)
{
    // enabled=false 是纯视频模式，此时采样率/声道数不参与校验
    if (params.enabled && (params.sampleRate <= 0 || params.channels <= 0)) {
        TG_LOGE("TGCapturer: SetupAudioEncoder 参数非法 sampleRate=%d channels=%d",
                params.sampleRate, params.channels);
        return TGErr_InvalidArgument;
    }
    m_audioParams = params;
    m_audioParamsSet = true;
    return TGErr_OK;
}

int TGCapturer::SetupVideoEncoder(const VideoEncoderParams& params)
{
    if (params.width <= 0 || params.height <= 0 || params.fps <= 0.0f || params.vBitrate <= 0) {
        TG_LOGE("TGCapturer: SetupVideoEncoder 参数非法 %dx%d fps=%.2f bitrate=%d", params.width,
                params.height, params.fps, params.vBitrate);
        return TGErr_InvalidArgument;
    }
    m_videoParams = params;
    m_videoParamsSet = true;
    m_perfMode = params.perfMode;
    return TGErr_OK;
}

int TGCapturer::SetupMuxer(const MuxerParams& params)
{
    if (params.muxerOutput.empty()) {
        TG_LOGE("TGCapturer: SetupMuxer 的 muxerOutput 为空");
        return TGErr_InvalidArgument;
    }
    m_muxParams = params;
    m_muxParamsSet = true;
    return TGErr_OK;
}

void TGCapturer::SetOnPreparedCallback(OnPreparedCallback cb)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onPrepared = std::move(cb);
}
void TGCapturer::SetOnErrorCallback(OnErrorCallback cb)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onError = std::move(cb);
}
void TGCapturer::SetOnStartCallback(OnStartCallback cb)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onStart = std::move(cb);
}
void TGCapturer::SetOnStopCallback(OnStopCallback cb)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onStop = std::move(cb);
}
void TGCapturer::SetOnNotifyCallback(OnNotifyCallback cb)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onNotify = std::move(cb);
}

void TGCapturer::SetPrivateData(const std::string& key, void* data)
{
    if (key == "capturer.conf" && data) {
        TG_LOGI("TGCapturer: 收到 capturer.conf（开播内核使用显式参数，conf 仅作记录）");
        return;
    }
    // Android：MediaProjection 实例（jobject，全局引用）由 App 侧授权后传入，
    // 屏幕采集与系统内录（AudioPlaybackCapture）都依赖它。
    if (key == "android.MediaProjection" && data) {
        m_mediaProjection = data;
        TG_LOGI("TGCapturer: 收到 android.MediaProjection");
        return;
    }
    // JavaVM：Android 侧 JNI 调用需要
#if defined(__ANDROID__)
    if (key == "android.JavaVM" && data) {
        platform::AndroidJni::SetJavaVM(static_cast<JavaVM*>(data));
        TG_LOGI("TGCapturer: 收到 android.JavaVM");
        return;
    }
#else
    (void)data;
#endif
}

// ------------------------------------------------------------------ 生命周期
// 生命周期入口只做"加锁 → 干活 → 解锁 → 通知"：宿主回调（Prepared / Error / Started / Stopped）
// 一律在解锁之后、经总线触发。之前回调在持锁状态下被调用，宿主若在回调里等待另一个
// 同样要调用本对象接口的线程，就会死锁。
// 回调仍然在调用方线程上同步执行（宿主在 Prepared 回调里调 Start 的用法保持不变）。
int TGCapturer::Prepare()
{
    PrepareError err;
    int ret;
    {
        std::unique_lock<std::mutex> lock(m_lifecycleMutex);
        // 等上一轮 Stop 彻底收尾：否则新一轮的 Prepared 事件可能排在上一轮的 Stopped 之前
        WaitStopSettled(lock);
        ret = PrepareLocked(err);
    }
    if (ret == 0) {
        m_bus.PublishSync(core::BusEventType::Prepared, "capturer", 0, {});
    } else {
        // 失败一律上报：返回值与回调 what 是同一个码，宿主两条路径择一处理即可
        m_bus.PublishSync(core::BusEventType::Error, "capturer", err.what, err.message, err.extra);
    }
    return ret;
}

int TGCapturer::PrepareLocked(PrepareError& err)
{
    // 检查与置位在同一个临界区内原子完成：并发两次 Prepare 只有一个能进
    State expected = State::Idle;
    if (!m_state.compare_exchange_strong(expected, State::Preparing)) {
        const int32_t cur = static_cast<int32_t>(m_state.load());
        TG_LOGW("TGCapturer: Prepare 状态错误 state=%d", cur);
        // 原先这条路径静默返回 -1：既不在枚举里、也不上报，宿主只能靠猜
        err = {TGErr_InvalidState, cur, "prepare called in wrong state"};
        return TGErr_InvalidState;
    }

    // 失败回滚：Prepare 中途失败时必须把已经打开的东西全部关掉再退回 Idle。
    // 原先只改状态就 return，后果是双重的：
    //   1) Stop() 在 Idle 态直接返回，StopLocked 永不执行 —— 宿主没有任何途径释放这些资源；
    //   2) 编码器 Open() 开头有 `if (m_opened) return 0`，残留的已打开编码器会在下一轮
    //      Prepare 被静默复用，新参数（分辨率/码率/采样率）被丢弃，表现为"第二次成功了但不生效"。
    auto fail = [&](TGCaptureError code, int32_t extra, const char* message) {
        err = {code, extra, message};
        TeardownLocked();
        m_state.store(State::Idle);
        return static_cast<int>(code);
    };

    if (!m_videoParamsSet || !m_audioParamsSet || !m_muxParamsSet) {
        return fail(TGErr_NotConfigured, 0, "encoders or muxer not configured");
    }

    // 视频类型：以配置为准，采集源尺寸作为兜底
    core::VideoMediaType videoType;
    videoType.SetWidth(m_videoParams.width);
    videoType.SetHeight(m_videoParams.height);
    videoType.SetPixelFormat(AV_PIX_FMT_NV12);
    videoType.SetFrameRate(static_cast<int>(m_videoParams.fps * 1000), 1000);

    // 分辨率必须是偶数：NV12/YUV420P 的 UV 是 2x2 下采样，奇数宽高会让 UV 平面整体错位，
    // 多数编码器也会直接拒绝。在这里对齐并说清楚，比让错误一路带到编码器好排查得多。
    const int evenW = m_videoParams.width > 0 ? (m_videoParams.width & ~1) : 0;
    const int evenH = m_videoParams.height > 0 ? (m_videoParams.height & ~1) : 0;
    if (evenW != m_videoParams.width || evenH != m_videoParams.height) {
        TG_LOGW("TGCapturer: 分辨率 %dx%d 不是偶数，已对齐为 %dx%d", m_videoParams.width,
                m_videoParams.height, evenW, evenH);
        m_videoParams.width = evenW;
        m_videoParams.height = evenH;
        videoType.SetWidth(evenW);
        videoType.SetHeight(evenH);
    }
    m_useExternalVideoPts = m_videoParams.videoUseExternalPts;
    {
        std::lock_guard<std::mutex> lock(m_externalAudioMutex);
        m_externalAudioChannels.clear();
    }

    // 纯视频模式：不建音频编码器、封装器也不建音频流
    m_audioEnabled = m_audioParams.enabled && m_audioParams.sampleRate > 0 && m_audioParams.channels > 0;

    // ---- 性能模式：GPU 链路需要采集侧与编码侧共用一个设备 ----
    void* externalDevice = nullptr;
#if defined(_WIN32)
    if (m_perfMode == PerformanceMode::Gpu) {
        auto* win = dynamic_cast<platform::WinScreenCapture*>(m_videoSource);
        // 必须是"屏幕"这种真有桌面复制通道的源：窗口采集走 GPU 会抓到整屏而不是目标窗口
        if (win && win->SupportsGpuDirect()) {
            win->SetGpuMode(true);
            externalDevice = win->EnsureD3D11Device();
            if (!externalDevice) {
                TG_LOGW("TGCapturer: GPU 采集不可用，回退 CPU 链路");
                win->SetGpuMode(false);
                m_perfMode = PerformanceMode::Cpu;
            }
        } else {
            TG_LOGW("TGCapturer: 当前采集源不支持 GPU 直连（窗口采集无桌面复制通道），回退 CPU 链路");
            m_perfMode = PerformanceMode::Cpu;
        }
    }
#endif
    // ---- 采集源先打开：GPU 模式要确认采集侧真的走的是硬件路径 ----
    if (m_videoSource) {
        const int openRet =
            m_videoSource->Open(m_videoParams.width, m_videoParams.height, m_videoParams.fps);
        if (openRet < 0) {
            // 外部注入源（上层自己推帧）的 Open 失败无所谓；但宿主显式创建的**原生**采集源
            // 打不开（设备被占用 / 权限未授予 / 无桌面复制通道）就必须让 Prepare 失败 ——
            // 原先只打一条 Warn 就继续，结果是"推流成功但永远没有画面"，是最难排查的那类故障。
            const VideoCapSourceType type = m_videoSource->GetVideoCapSourceType();
            if (type == TypeOfGameEngine4Slave) {
                TG_LOGW("TGCapturer: 视频源打开失败 ret=%d（外部注入模式，由上层推帧）", openRet);
            } else {
                TG_LOGE("TGCapturer: 原生视频源打开失败 type=%d ret=%d", static_cast<int>(type),
                        openRet);
                return fail(TGErr_OpenVideoSource, openRet, "open video capture source failed");
            }
        }
    }
#if defined(_WIN32)
    if (m_perfMode == PerformanceMode::Gpu) {
        auto* win = dynamic_cast<platform::WinScreenCapture*>(m_videoSource);
        if (win && win->GpuMode() && win->FrameWidth() > 0 && win->FrameHeight() > 0) {
            // GPU 链路在显存里只做同尺寸拷贝，不做缩放：
            // 因此采集分辨率即编码分辨率（否则会变成"裁剪"而不是"缩放"）。
            m_videoParams.width = win->FrameWidth();
            m_videoParams.height = win->FrameHeight();
            videoType.SetWidth(win->FrameWidth());
            videoType.SetHeight(win->FrameHeight());
            TG_LOGI("TGCapturer: GPU 模式按采集分辨率编码 %dx%d",
                    win->FrameWidth(), win->FrameHeight());
        }
        const bool gpuReady = win && win->GpuMode() && win->ActiveMode() == DXGI && win->D3D11Device();
        if (!gpuReady) {
            TG_LOGW("TGCapturer: 采集未走 GPU 路径，编码器一并回退 CPU 链路");
            m_perfMode = PerformanceMode::Cpu;
            externalDevice = nullptr;
            if (win) {
                win->SetGpuMode(false);
            }
        }
    }
#endif
    m_videoEncoder->SetPerformanceMode(m_perfMode);
    m_videoEncoder->SetExternalDevice(externalDevice);

    // ---- 音画同步初始化 ----
    core::VideoPtsParams syncParams;
    syncParams.maxLeadUs = m_videoParams.videoMaxLeadUs > 0 ? m_videoParams.videoMaxLeadUs : 500000;
    m_videoPtsGen->SetParams(syncParams);
    m_videoPtsGen->Reset();
    m_audioClock->Reset();
    m_audioClock->SetCompensationUs(m_videoParams.audioCompensationUs);
    m_videoOnlyTimeline->Reset();
    m_videoIntervalUs = m_videoParams.fps > 0 ? static_cast<int64_t>(1000000.0 / m_videoParams.fps) : 33333;
    m_syncDropped.store(0);
    m_bpDropped.store(0);
    m_audioWaitStartUs = 0;
    m_lastVideoPtsUs = -1;
    m_lastVideoCaptureUs = -1;
    // 首帧标志必须复位，否则第二轮不再触发 EsCap_Notify_Fisrt_Frame_OK
    m_firstFrameSent.store(false);
    m_lastRuntimeError.store(0);
    // 断流看护的阈值在这里取用，Reset 留到 Start（Prepare 到 Start 之间的空档不算断流）
    m_videoWatch.Configure(m_videoTimeoutMs.load() * 1000);
    m_audioWatch.Configure(m_audioEnabled ? m_audioTimeoutMs.load() * 1000 : 0);
    // 编码器侧的"落后丢帧"只是最后一道兜底：阈值必须落在 VideoPtsGenerator 的
    // ForceAlign 区间之外（forceSyncFactor × 帧间隔 × 1.5）。
    // 否则会在 RangeAdapt 正常追赶期间（66~200ms @30fps）白白砍帧率，
    // 而时间轴并不受影响（PTS 由 Next() 独立推进），等于纯损失。
    const int64_t autoMaxLagUs = syncParams.forceSyncFactor * m_videoIntervalUs * 3 / 2;
    m_videoEncoder->SetMaxLagUs(m_videoParams.videoMaxLagUs > 0 ? m_videoParams.videoMaxLagUs
                                                               : autoMaxLagUs);

    // 画面变换：采集帧进入编码器之前统一应用（裁剪 → 翻转 → 旋转 → 缩放 → 格式）
    core::VideoTransform transform;
    transform.rotation = m_videoParams.videoRotation;
    transform.flipHorizontal = m_videoParams.videoFlipHorizontal;
    transform.flipVertical = m_videoParams.videoFlipVertical;
    transform.cropX = m_videoParams.videoCropX;
    transform.cropY = m_videoParams.videoCropY;
    transform.cropWidth = m_videoParams.videoCropWidth;
    transform.cropHeight = m_videoParams.videoCropHeight;
    m_videoEncoder->SetVideoTransform(transform);
    m_videoEncoder->SetGpuReadbackFallback(m_videoParams.gpuAllowReadbackFallback);
    if (!transform.IsIdentity()) {
        int outW = 0, outH = 0;
        transform.TransformedSize(m_videoParams.width, m_videoParams.height, outW, outH);
        TG_LOGI("TGCapturer: 画面变换 rot=%d flipH=%d flipV=%d crop=%d,%d %dx%d -> %dx%d",
                transform.NormalizedRotation(), transform.flipHorizontal ? 1 : 0,
                transform.flipVertical ? 1 : 0, transform.cropX, transform.cropY,
                transform.cropWidth, transform.cropHeight, outW, outH);
        if (m_perfMode == PerformanceMode::Gpu) {
            // 纯显存的 CopySubresourceRegion 只能做 1:1 拷贝，带不了旋转/翻转/缩放，
            // 所以 GPU 模式下一旦开变换就只能把纹理读回内存用 libyuv 处理 ——
            // 代价是每帧几十 MB 的回读，帧率会大幅下降。这里必须显式告知。
            TG_LOGW("TGCapturer: GPU 模式下启用画面变换会强制回读显存，帧率将显著下降；"
                    "若不需要变换请清空相关参数，或把 gpuAllowReadbackFallback 设为 false 直接丢帧");
        }
    }
    m_muxer->SetInterleaveUs(static_cast<int64_t>(m_muxParams.interleaveMs) * 1000);

    core::AudioMediaType audioType;
    audioType.SetSampleRate(m_audioParams.sampleRate);
    audioType.SetChannels(m_audioParams.channels);
    audioType.SetSampleFormat(AV_SAMPLE_FMT_S16);
    audioType.SetFrameSize(1024);

    if (m_videoEncoder->Open(m_videoParams, videoType) < 0) {
        return fail(TGErr_OpenVideoEncoder, m_videoEncoder->LastAvError(),
                    "open video encoder failed");
    }
    // 编码器没能建起硬件链路（hwdevice / hw_frames_ctx 初始化失败）时，采集侧必须一并退回系统内存。
    // 否则采集继续产出 GPU 纹理帧，而编码器走软件路径，BuildAVFrame 拿不到 planes[0]，
    // 结果是每帧被丢弃 —— 表现为"流能开、包在写、画面全黑"，且没有任何显式报错。
#if defined(_WIN32)
    if (m_perfMode == PerformanceMode::Gpu && !m_videoEncoder->IsGpuPath()) {
        TG_LOGW("TGCapturer: 编码器硬件链路未就绪，采集侧一并回退 CPU（帧不再走显存）");
        auto* win = dynamic_cast<platform::WinScreenCapture*>(m_videoSource);
        if (win) {
            win->SetGpuMode(false);
        }
        m_perfMode = PerformanceMode::Cpu;
    }
#endif
    if (m_audioEnabled) {
        if (m_audioEncoder->Open(m_audioParams, audioType) < 0) {
            return fail(TGErr_OpenAudioEncoder, m_audioEncoder->LastAvError(),
                        "open audio encoder failed");
        }
    } else {
        TG_LOGI("TGCapturer: 纯视频模式（audio disabled）");
    }

    if (m_muxer->Open(m_muxParams, m_videoEncoder->CodecContext(),
                      m_audioEnabled ? m_audioEncoder->CodecContext() : nullptr) < 0) {
        return fail(TGErr_OpenMuxer, m_muxer->LastAvError(), "open muxer failed");
    }

    if (m_audioEnabled) {
        for (auto* source : m_audioSources) {
            if (source && source->Open(m_audioParams.sampleRate, m_audioParams.channels) < 0) {
                // 音频按"降级而非失败"处理（少一路声音仍可开播），但不能就这么算了：
                // 开播后若混流器整体没有音频"输出"，断流看护会上报 TGErr_AudioCaptureStalled。
                // 注意：部分平台的采集源在打开失败后仍会注入静音帧，此时混流照常出帧，
                // 看护探测不到——这种"有声道但全静音"的情况只能靠日志排查，无法自动上报。
                TG_LOGW("TGCapturer: 音频源 type=%d 打开失败，该路按静音处理",
                        static_cast<int>(source->GetAudioCapSourceType()));
            }
        }
    }

    // 混流通道：每一路音频源一条，输出格式取编码器配置（单路也同样建，以便做时钟校正）
    if (m_audioEnabled) {
        m_mixer->Reset();
        m_mixer->SetOutputFormat(m_audioParams.sampleRate, m_audioParams.channels);
        const int64_t nowUs = core::MonotonicClock::NowUs();
        for (size_t i = 0; i < m_audioSources.size(); ++i) {
            m_mixer->AddChannel(static_cast<int>(i), m_audioParams.sampleRate,
                                m_audioParams.channels, nowUs);
        }
        m_audioFrameSize = audioType.FrameSize() > 0 ? audioType.FrameSize() : 1024;
    }

    m_state.store(State::Prepared);
    return 0;
}

int TGCapturer::Start()
{
    PrepareError err;
    int ret;
    {
        std::unique_lock<std::mutex> lock(m_lifecycleMutex);
        WaitStopSettled(lock);
        State expected = State::Prepared;
        if (!m_state.compare_exchange_strong(expected, State::Starting)) {
            const int32_t cur = static_cast<int32_t>(m_state.load());
            TG_LOGW("TGCapturer: Start 状态错误 state=%d", cur);
            // 不在这里 PublishSync：它是同步派发，会在**持有 m_lifecycleMutex 的情况下**
            // 直接执行宿主的 onError。宿主在回调里调 Stop()/Prepare() 就会重入这把锁，
            // 或者等另一个正在调生命周期接口的线程 —— 跨线程死锁。统一到锁外上报。
            err = {TGErr_InvalidState, cur, "start called in wrong state"};
            ret = TGErr_InvalidState;
        } else {
            ret = StartLocked(err);
        }
    }
    if (ret == 0) {
        m_bus.PublishSync(core::BusEventType::Started, "capturer", 0, {});
    } else {
        m_bus.PublishSync(core::BusEventType::Error, "capturer", err.what, err.message, err.extra);
    }
    return ret;
}

// 线程起不来意味着这一轮根本不会出流（通常是上一轮没停干净）：
// 原先只打一条 Warn 然后照常返回 0，宿主会以为开播成功
int TGCapturer::StartLocked(PrepareError& err)
{
    auto fail = [&](int32_t stage, const char* what) {
        TG_LOGE("TGCapturer: %s 线程启动失败（上一轮可能未停止干净）", what);
        err = {TGErr_StartFailed, stage, std::string("start failed: ") + what};
        // 逆序停掉本轮已经起来的线程：只退状态不停线程的话，muxer / 视频编码线程会继续空转，
        // 宿主若不 Stop 直接析构，它们就会在正在销毁的成员上继续发包
        if (m_audioEncoder) {
            m_audioEncoder->Stop();
        }
        if (m_videoEncoder) {
            m_videoEncoder->Stop();
        }
        if (m_muxer) {
            m_muxer->Stop();
        }
        m_state.store(State::Prepared);   // 退回可重试的状态，宿主可以 Stop 后重开
        return static_cast<int>(TGErr_StartFailed);
    };
    if (!m_muxer->Start()) {
        return fail(1, "muxer");
    }
    if (!m_videoEncoder->Start()) {
        return fail(2, "video encoder");
    }
    if (m_audioEnabled && !m_audioEncoder->Start()) {
        return fail(3, "audio encoder");
    }
    if (m_videoSource) {
        m_videoSource->StartCapture();
    }
    if (m_audioEnabled) {
        for (auto* source : m_audioSources) {
            if (source) {
                source->StartCapture();
            }
        }
        StartAudioMixer();   // 多于一路时合并（麦克风 + 扬声器）
    }

    m_state.store(State::Running);
    StartWatchdog();
    TG_LOGI("TGCapturer: 已启动");
    return TGErr_OK;
}

void TGCapturer::Stop()
{
    {
        // 与 Prepare / Start 共用同一把锁：三者不会交错执行，
        // 从根上消除"Stop 跑完时 Start 还没启动线程"造成的野线程。
        std::unique_lock<std::mutex> lock(m_lifecycleMutex);
        if (!WaitStopSettled(lock)) {
            return;   // 本线程就是正在停机的那个（宿主在 OnStop 里又调了 Stop）
        }
        if (m_state.load() == State::Idle) {
            return;
        }
        StopLocked();
        // 标记"停机尚未收尾"后才放锁：别的线程此刻虽能拿到锁，
        // 但会在 WaitStopSettled 上等到收尾结束，不会误判成"已经停干净了"
        m_stopping = true;
        m_stopOwner = std::this_thread::get_id();
    }
    // 解锁之后：先让运行期积压的错误 / 通知被宿主看到，再告知已停止 ——
    // 保证宿主看到的顺序是"…错误…Stopped"，而不是 Stopped 之后又冒出错误。
    // （本函数若就在事件分发线程里被调用，Drain 会直接返回，不会等自己）
    m_bus.Events().Drain();
    // code = 本轮最后一次上报的错误码：OnStopCallback(err) 这个参数以前恒为 0，等于废的
    m_bus.PublishSync(core::BusEventType::Stopped, "capturer", m_lastRuntimeError.load(), {});
    {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        m_stopping = false;
        m_stopOwner = {};
    }
    m_stopSettled.notify_all();
}

// 返回 false 表示"调用方自己就是正在停机的线程"，不应重复停。
// 其余情况返回时保证没有别的线程正在停机（含锁外收尾）。
bool TGCapturer::WaitStopSettled(std::unique_lock<std::mutex>& lock)
{
    if (m_stopping && m_stopOwner == std::this_thread::get_id()) {
        return false;
    }
    m_stopSettled.wait(lock, [this] { return !m_stopping; });
    return true;
}

void TGCapturer::TeardownLocked()
{
    // 先停看护：拆除过程中采集必然停止产出，否则会报出一个无意义的"断流"
    StopWatchdog();

    // 封装层正在持续写失败（网络已断）：直接打断 I/O，别让收尾（写尾、关闭连接）再各等一个超时
    if (m_muxer && m_muxer->IsFailing()) {
        m_bus.Send("muxer", core::BusCommand{core::BusCommandType::Abort, 0});
    }

    // 按数据流向逆序停止：先停采集，再 drain 编码器，最后收尾封装。
    // StopCapture 现在是真正的回调栅栏（关闸 + join + 等在飞回调），返回后
    // 下面拆编码器 / 混流器时不会再有采集线程闯进来。
    if (m_videoSource) {
        m_videoSource->StopCapture();
    }
    // 不按 m_audioEnabled 过滤：Prepare 中途失败时它可能还是上一轮的值，
    // 而已经 Open 过的音频源无论如何都得停。对没启动的源是无副作用的空操作。
    for (auto* source : m_audioSources) {
        if (source) {
            source->StopCapture();
        }
    }
    StopAudioMixer();
    if (m_videoEncoder) {
        m_videoEncoder->Stop();
    }
    if (m_audioEncoder) {
        m_audioEncoder->Stop();
    }
    if (m_muxer) {
        m_muxer->Stop();
        m_muxer->Close();
    }
    if (m_videoEncoder) {
        m_videoEncoder->Close();
    }
    if (m_audioEncoder) {
        m_audioEncoder->Close();
    }
    // 采集源的设备资源（摄像头/麦克风句柄、COM 对象、D3D 纹理、GDI 位图）最后放。
    // 此时采集线程已 join、在飞回调已退出，Close 不会与产帧并发。
    // 这一步以前完全缺失：设备只在采集源析构时才释放，而采集源是宿主持有的，
    // 于是"Stop 之后设备仍被占用"，宿主换一路源重开就会撞上"设备被占用"。
    if (m_videoSource) {
        m_videoSource->Close();
    }
    for (auto* source : m_audioSources) {
        if (source) {
            source->Close();
        }
    }
    m_sync->Reset();
}

void TGCapturer::StopLocked()
{
    m_state.store(State::Stopping);
    TeardownLocked();
    if (m_videoPtsGen) {
        const auto st = m_videoPtsGen->stats();
        TG_LOGI("TGCapturer: 音画同步 avDiff=%lldms clamp=%llu forceSync=%llu adapt=%llu stall=%llu skip=%llu jump=%llu staleReset=%llu syncDrop=%llu bpDrop=%llu",
                static_cast<long long>(st.avDiffUs / 1000),
                static_cast<unsigned long long>(st.clamped),
                static_cast<unsigned long long>(st.forceSynced),
                static_cast<unsigned long long>(st.adapted),
                static_cast<unsigned long long>(st.stalled),
                static_cast<unsigned long long>(st.skipped),
                static_cast<unsigned long long>(st.jumpWarns),
                static_cast<unsigned long long>(st.staleResets),
                static_cast<unsigned long long>(m_syncDropped.load()),
                static_cast<unsigned long long>(m_bpDropped.load()));
    }
    m_state.store(State::Idle);
    TG_LOGI("TGCapturer: 已停止");
}

// ------------------------------------------------------------------ 断流看护
// 采集类故障（设备被拔 / 权限被撤销 / DXGI 重建失败 / 上层停止推帧）在各平台的表现
// 五花八门，但共同点是"不再有数据"。与其让每个平台实现各自上报（三端都要改、还容易漏），
// 不如在内核侧统一按"多久没数据"判定 —— 顺带让 SetAVTimeout 这个接口真正生效。
void TGCapturer::StartWatchdog()
{
    if (m_watchThread.joinable()) {
        return;
    }
    const int64_t nowUs = core::MonotonicClock::NowUs();
    m_videoWatch.Reset(nowUs);
    m_audioWatch.Reset(nowUs);
    m_watchThread = std::jthread([this](std::stop_token st) { WatchLoop(st); });
}

void TGCapturer::StopWatchdog()
{
    if (m_watchThread.joinable()) {
        m_watchThread.request_stop();
        m_watchThread.join();
    }
}

void TGCapturer::WatchLoop(std::stop_token token)
{
    // 轮询周期取 200ms：断流阈值以秒计，这个粒度足够，且几乎不占 CPU
    constexpr auto kTick = std::chrono::milliseconds(200);
    int64_t videoStallSinceUs = 0;
    int64_t audioStallSinceUs = 0;

    auto handle = [&](core::StallWatch& watch, int64_t nowUs, int64_t& stallSinceUs,
                      int32_t errCode, NotifyMsgType recovered, const char* name) {
        switch (watch.Poll(nowUs)) {
            case core::StallWatch::Transition::Stalled: {
                const int64_t idleMs = watch.IdleUs(nowUs) / 1000;
                stallSinceUs = nowUs - watch.IdleUs(nowUs);
                char message[160];
                std::snprintf(message, sizeof(message), "%s capture stalled for %lldms", name,
                              static_cast<long long>(idleMs));
                FireError(errCode, static_cast<int32_t>(idleMs), message);
                break;
            }
            case core::StallWatch::Transition::Recovered: {
                const int64_t downMs =
                    stallSinceUs > 0 ? (nowUs - stallSinceUs) / 1000 : 0;
                TG_LOGI("TGCapturer: %s 采集已恢复（断流 %lldms）", name,
                        static_cast<long long>(downMs));
                FireNotify(recovered, "capture recovered", downMs);
                stallSinceUs = 0;
                break;
            }
            case core::StallWatch::Transition::None:
                break;
        }
    };

    while (!token.stop_requested()) {
        std::this_thread::sleep_for(kTick);
        if (token.stop_requested() || m_state.load() != State::Running) {
            continue;
        }
        const int64_t nowUs = core::MonotonicClock::NowUs();
        handle(m_videoWatch, nowUs, videoStallSinceUs, TGErr_VideoCaptureStalled,
               EsCap_Notify_VideoCapture_Recovered, "video");
        handle(m_audioWatch, nowUs, audioStallSinceUs, TGErr_AudioCaptureStalled,
               EsCap_Notify_AudioCapture_Recovered, "audio");
    }
}

void TGCapturer::ReleaseAll()
{
    std::unique_lock<std::mutex> lock(m_lifecycleMutex);
    // 析构路径：必须等任何在途的 Stop 收尾完，否则这里开始销毁成员时
    // 那个线程还在 PublishSync 里回调宿主
    WaitStopSettled(lock);
    m_ownedVideoSources.clear();
    m_ownedAudioSources.clear();
    m_videoSource = nullptr;
    m_audioSource = nullptr;
    m_audioSources.clear();
    if (m_audioClock) {
        m_audioClock->Reset();
    }
    if (m_mixer) {
        m_mixer->Reset();
    }
}

// ------------------------------------------------------------------ 帧流转
void TGCapturer::OnVideoFrame(core::FramePtr frame)
{
    if (!frame || m_state.load() != State::Running) {
        return;
    }
    // 看护判定的是"采集端还在不在产出"，而不是"这一帧最终有没有被编码"：
    // 下面的 Mute 丢帧、硬件帧无法改写、音频主时钟未就绪而丢弃等分支都会提前 return，
    // 但这些都不是采集故障，打点必须在它们之前，否则会误报断流。
    m_videoWatch.OnActivity(core::MonotonicClock::NowUs());

    if (m_videoMuted.load()) {
        // 对齐 MediaX：MuteVideo 期间若设置了 pause 图片，用它替换像素但保留 PTS 继续推进；
        // 没设置图片就"冻结最后一帧"—— 当前 frame 本身即是最新一帧，拿去编码等效于冻结。
        // 没有 pauseFrame 且没有有效像素时才真丢弃（硬件直通路径下无法替换）。
        if (m_pauseFrame && m_pauseFrame->Valid() && frame->planes[0]) {
            if (!m_pauseFrame->ApplyTo(*frame)) {
                return;
            }
        } else if (!frame->planes[0]) {
            return;   // 硬件帧 + 无 pause 图片：无法替换，只能丢
        }
        // 没有 pauseFrame：原封不动把本帧交给下游（视觉上即"冻结"）。
    }

    // 水印：仅对 CPU 帧生效；硬件直通帧跳过并限频提示（后续如需 GPU 水印可扩展 D3D11/Metal 分支）
    if (m_watermark && !m_watermark->Empty()) {
        if (frame->planes[0] && frame->pixFmt == AV_PIX_FMT_NV12) {
            m_watermark->BlendOnto(*frame);
        } else if (frame->hwHandle) {
            TG_LOGW_THROTTLED(3000000, "TGCapturer: 硬件帧路径暂不支持水印，本帧水印已跳过");
        }
    }

    // 背压：编码器积压过多则丢帧，保证直播实时性
    constexpr size_t kMaxEncoderPending = 8;
    const size_t encoderPending = m_videoIn->Pending();
    if (encoderPending >= kMaxEncoderPending) {
        const auto n = m_bpDropped.fetch_add(1) + 1;
        TG_LOGW_THROTTLED(2000000, "TGCapturer: 编码器积压 %zu，丢帧（编码跟不上采集，bpDrop=%llu）",
                          encoderPending, static_cast<unsigned long long>(n));
        return;
    }

    // 调用方自带时间轴：直接用它给的 PTS，不再走内核的同步与时间轴生成
    if (frame->ptsLocked && frame->ptsUs >= 0) {
        const int frameW = frame->width;
        const int frameH = frame->height;
        const int64_t framePts = frame->ptsUs;
        if (!m_videoIn->Submit(std::move(frame))) {
            return;
        }
        if (!m_firstFrameSent.exchange(true)) {
            TG_LOGI("TGCapturer: 首帧视频 %dx%d pts=%lld（外部时间轴）", frameW, frameH,
                    static_cast<long long>(framePts));
            FireNotify(EsCap_Notify_Fisrt_Frame_OK, "first frame", 0);
        }
        return;
    }

    // ---- 音画同步：视频 PTS 由音频时钟反推（不用采集 wall clock）----
    // 对齐 MediaX：每帧独立判定，不保留"已降级"状态 ——
    // 音频恢复后下一帧 m_audioClock->IsFresh() 自然为 true，直接切回主时钟；
    // 两条时间轴切换时都从最后一个已输出帧接续（见 m_lastVideoPtsUs），保证 PTS 不回退。
    //
    // 关键：判活用 IsFresh(audioTimeoutMs) 而不是 Valid()。Valid() 是单向开关
    // （第一次 Advance/Reserve 后永远 true），音频长时间断流时 ReferencePtsUs()
    // 会一直返回最后一次推进的旧 PTS —— VideoPtsGenerator 的停滞判定虽然能
    // clamp 到 audio+maxLead，但那个 audio 已经是几秒钟前的值，视频 PTS 会相对
    // 墙钟永久落后一个 drift。IsFresh 按"最近 N ms 内有过真实推进"判活，断流后
    // 会自然走到下面的 wall-clock fallback 分支。
    //
    // 有音频时"视频等音频"：主时钟尚未产出第一拍就先不发帧（最多等 audioTimeoutMs）。
    // 等不到才走采集时间轴 —— 这是 wall-clock fallback 分支，不污染音频时钟域。
    int64_t audioRef = core::kInvalidPtsUs;
    bool useAudioClock = false;
    if (m_audioEnabled && m_audioClock && m_audioClock->IsFresh(m_audioTimeoutMs)) {
        useAudioClock = true;
        audioRef = m_audioClock->ReferencePtsUs();
        // 音频刚从异常恢复：重置等待起点；VideoPtsGenerator 接续 fallback 期间已输出的 PTS
        // （它的内部 PTS 还停在断流前，直接用会产出回退 PTS，被编码器整段丢弃）
        if (m_audioWaitStartUs != 0) {
            TG_LOGI("TGCapturer: 音频主时钟恢复，切回主时钟（曾等待/断流 %lldms，接续 pts=%lld）",
                    static_cast<long long>((core::MonotonicClock::NowUs() - m_audioWaitStartUs) / 1000),
                    static_cast<long long>(m_lastVideoPtsUs));
            m_audioWaitStartUs = 0;
            m_videoPtsGen->Rebase(m_lastVideoPtsUs);
            m_videoOnlyTimeline->Reset();
        }
    } else if (m_audioEnabled) {
        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (m_audioWaitStartUs == 0) {
            m_audioWaitStartUs = nowUs;
            // 区分"首帧前等待" vs "运行中断流"的日志，便于定位
            if (m_audioClock && m_audioClock->Valid()) {
                TG_LOGW("TGCapturer: 音频主时钟断流（超过 %lldms 未推进），进入 wall-clock fallback，接续 pts=%lld",
                        static_cast<long long>(m_audioTimeoutMs), static_cast<long long>(m_lastVideoPtsUs));
            } else {
                TG_LOGI("TGCapturer: 等待音频主时钟首帧（最多 %lldms）",
                        static_cast<long long>(m_audioTimeoutMs));
            }
            // 运行中断流：wall-clock 时间轴从最后一个已输出帧接续（pts + 采集时刻差），
            // 而不是从 0 重起 —— 从 0 起会比已输出的 PTS 小，整段被编码器按"PTS 回退"丢掉
            if (m_lastVideoPtsUs >= 0 && m_lastVideoCaptureUs >= 0) {
                m_videoOnlyTimeline->Rebase(m_lastVideoPtsUs, m_lastVideoCaptureUs);
            } else {
                m_videoOnlyTimeline->Reset();
            }
        }
        // 首帧前（Valid()==false）要尊重 audioTimeoutMs 的等待窗口；
        // 运行中断流（Valid()==true 但 IsFresh()==false）直接 fallback，不再等。
        const bool firstFrameWait = m_audioClock && !m_audioClock->Valid();
        if (firstFrameWait && (nowUs - m_audioWaitStartUs < m_audioTimeoutMs * 1000)) {
            return;   // 正常等待，不计入丢帧统计
        }
        TG_LOGW_THROTTLED(5000000, "TGCapturer: 音频未就绪/断流，本帧走采集时间轴");
        // 注意：不再调 m_sync->Reset()。
        // 当前帧用 wall-clock PTS，但 m_sync 里的旧 audio ref 已被 VideoEncoder 的落后丢帧判决所依赖；
        // 恢复音频主时钟后，上面 IsFresh 分支会接管，m_sync 由 UpdateAudioClock 刷新即可。
    }

    if (useAudioClock) {
        const int64_t pts = m_videoPtsGen->Next(audioRef, m_videoIntervalUs, true, m_audioClock->IdleUs());
        if (pts == core::kInvalidPtsUs) {
            // 音频停滞已到超前上限 / 视频突发超前音频：本帧不编码
            const uint64_t dropped = m_syncDropped.fetch_add(1) + 1;
            TG_LOGW_THROTTLED(2000000, "TGCapturer: 视频超前音频（突发到达或音频停滞），丢帧（syncDrop=%llu）",
                              static_cast<unsigned long long>(dropped));
            return;
        }
        frame->ptsUs = pts;
        if (m_sync) {
            m_sync->UpdateAudioClock(audioRef);   // 供 VideoEncoder 的"落后丢帧"兜底
        }
    } else {
        // 无音频 / 音频超时：走采集时间轴（首帧归零，单调递增）
        const int64_t pts = m_videoOnlyTimeline->Next(frame->captureUs);
        frame->ptsUs = pts >= 0 ? pts : frame->captureUs;
    }

    // 注意：frame 交给总线后会被移动，之后不可再访问其成员
    const int width = frame->width;
    const int height = frame->height;
    const int64_t pts = frame->ptsUs;
    const int64_t captureUs = frame->captureUs;

    if (!m_videoIn->Submit(std::move(frame))) {
        return;
    }
    if (pts >= 0) {
        m_lastVideoPtsUs = pts;
        m_lastVideoCaptureUs = captureUs;
    }

    if (!m_firstFrameSent.exchange(true)) {
        TG_LOGI("TGCapturer: 首帧视频 %dx%d pts=%lld", width, height, static_cast<long long>(pts));
        FireNotify(EsCap_Notify_Fisrt_Frame_OK, "first frame", 0);
    }
}

// 外部注入的音频：同样进混流器（走 0 号通道，没有内部音频源时由它独占）
void TGCapturer::OnAudioFrame(AudioCapSourceType type, core::FramePtr frame)
{
    if (!frame || !m_audioEnabled || !m_mixer || m_state.load() != State::Running) {
        return;
    }
    if (!frame->audioData || frame->audioBytes == 0 || frame->channels <= 0) {
        return;
    }
    // 外部注入的音频按 type 分路：每种 AudioCapSourceType 独占一个混流通道。
    // 共享单一通道会把两路串成一条时间轴（表现为"交替输出"而非真正混音），且与内部麦克风源
    // 共用同一个通道时钟还会互相拉扯时间戳（音频抖动/变调）。
    int id = -1;
    {
        std::lock_guard<std::mutex> lock(m_externalAudioMutex);
        const int key = static_cast<int>(type);
        auto it = m_externalAudioChannels.find(key);
        if (it != m_externalAudioChannels.end()) {
            id = it->second;
        } else {
            const int newId = static_cast<int>(m_mixer->ChannelCount());  // 追加在内部源之后
            // 以第一个 active 通道（内部源或首路外部注入）的格式作为混流输出格式。
            if (newId == 0) {
                m_mixer->SetOutputFormat(frame->sampleRate, frame->channels);
            }
            if (m_mixer->AddChannel(newId, frame->sampleRate, frame->channels,
                                    core::MonotonicClock::NowUs()) == 0) {
                m_externalAudioChannels[key] = newId;
                id = newId;
                TG_LOGI("TGCapturer: 外部注入音频 type=%d 占用混流通道 %d（%dHz %dch）",
                        static_cast<int>(type), id, frame->sampleRate, frame->channels);
            } else {
                // 通道建不起来 = 这一路音频被永久丢弃，原先静默 return，宿主完全无感
                TG_LOGE("TGCapturer: 为外部音频 type=%d 创建混流通道失败",
                        static_cast<int>(type));
                FireError(TGErr_Internal, 0, "add audio mixer channel failed");
                return;
            }
        }
    }
    if (id < 0) {
        return;
    }
    const int frames = static_cast<int>(frame->audioBytes / (sizeof(int16_t) * static_cast<size_t>(frame->channels)));
    m_mixer->Push(id, reinterpret_cast<const int16_t*>(frame->audioData.get()), frames,
                  frame->captureUs);
}

// ------------------------------------------------------------------ 外部注入
int TGCapturer::PushExternalVideoFrame(const uint8_t* data, int width, int height, int stride,
                                       int64_t ptsUs, void* pixelBuffer)
{
    if (m_state.load() != State::Running) {
        return TGErr_InvalidState;
    }
    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Video;
    frame->width = width;
    frame->height = height;
    frame->pixFmt = AV_PIX_FMT_NV12;
    frame->ptsUs = ptsUs >= 0 ? ptsUs : core::MonotonicClock::NowUs();
    frame->captureUs = core::MonotonicClock::NowUs();

    if (pixelBuffer) {
        // iOS CVPixelBuffer / Windows D3D 纹理：零拷贝直送硬编。
        // 两种载体共用同一个入口，必须按平台选定 hwKind —— 之前恒定为 CVPixelBuffer，
        // Windows 上传 D3D 纹理会匹配不上 HwKind::D3D11Texture，帧被整条链路丢弃。
        frame->hwHandle = pixelBuffer;
#if defined(__APPLE__)
        frame->hwKind = core::HwKind::CVPixelBuffer;
#elif defined(_WIN32)
        frame->hwKind = core::HwKind::D3D11Texture;
#else
        frame->hwKind = core::HwKind::None;
        TG_LOGW_THROTTLED(3000000, "TGCapturer: 当前平台不支持外部注入硬件帧，该帧按无效处理");
        return TGErr_NotSupported;
#endif
        // 硬件帧没有系统内存像素，pixFmt 无意义（与采集侧 GPU 帧保持一致）
        frame->pixFmt = AV_PIX_FMT_NONE;
    } else if (data) {
        if (width <= 0 || height <= 0) {
            return TGErr_InvalidArgument;
        }
        const int rowStride = stride > 0 ? stride : width;
        if (rowStride < width) {
            // stride 不能小于宽度，否则 Y 平面行会重叠
            return TGErr_InvalidArgument;
        }
        // NV12 是两平面：Y(stride × h) + UV(stride × h/2)
        const size_t ySize = static_cast<size_t>(rowStride) * height;
        const size_t uvSize = static_cast<size_t>(rowStride) * (height / 2);
        auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize],
                                               std::default_delete<uint8_t[]>());
        memcpy(buffer.get(), data, ySize + uvSize);
        uint8_t* uv = buffer.get() + ySize;
        frame->planes[0] = buffer;
        frame->strides[0] = rowStride;
        // 关键：必须同时给出 UV 平面。之前只填了 planes[0]，
        // sws_scale 会拿到 planes[1] == nullptr，导致画面只剩亮度（绿屏/崩溃）。
        frame->planes[1] = std::shared_ptr<uint8_t>(buffer, uv);
        frame->strides[1] = rowStride;
        frame->width = width & ~1;
        frame->height = height & ~1;
    } else {
        return TGErr_InvalidArgument;   // data 与 pixelBuffer 都为空
    }
    // 调用方自带时间轴时锁定 PTS，内核不再用音频主时钟改写（见 videoUseExternalPts）
    frame->ptsLocked = m_useExternalVideoPts && ptsUs >= 0;
    OnVideoFrame(std::move(frame));
    return TGErr_OK;
}

int TGCapturer::PushExternalAudioFrame(AudioCapSourceType type, const uint8_t* data, int size,
                                       int sampleRate, int channels, int64_t ptsUs)
{
    if (m_state.load() != State::Running) {
        return TGErr_InvalidState;
    }
    if (!data || size <= 0 || sampleRate <= 0 || channels <= 0) {
        // 原先这些参数错误与"未 Start"共用 -1，宿主无法区分是时序问题还是参数问题
        return TGErr_InvalidArgument;
    }
    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Audio;
    frame->sampleRate = sampleRate;
    frame->channels = channels;
    frame->sampleFmt = AV_SAMPLE_FMT_S16;
    frame->audioBytes = static_cast<size_t>(size);
    frame->audioData = std::shared_ptr<uint8_t>(new uint8_t[static_cast<size_t>(size)], std::default_delete<uint8_t[]>());
    memcpy(frame->audioData.get(), data, static_cast<size_t>(size));
    // ptsUs：调用方自带的采集时间戳。既写进 frame->ptsUs（供 Encoder 用），
    // 也写进 frame->captureUs（供 Mixer 的 ChannelClock 做 drift 校正）。
    // 之前只写了 ptsUs，没写 captureUs → Mixer 走兜底用 wallclock，调用方 PTS 实际被丢弃
    // （PTS 抖动 / 回退 / 跳变全被内核无感吸收，故障完全不可观测）。
    // 对齐 PushExternalVideoFrame 的 captureUs 处理，让两条外灌通路语义一致。
    const int64_t effectivePts = ptsUs >= 0 ? ptsUs : core::MonotonicClock::NowUs();
    frame->ptsUs = effectivePts;
    frame->captureUs = effectivePts;
    OnAudioFrame(type, std::move(frame));
    return TGErr_OK;
}

// ------------------------------------------------------------------ 运行期控制
bool TGCapturer::MuteAudio(bool mute, AudioCapSourceType channel)
{
    // 对齐 MediaX::CESAudioMixer::MuteCapture：按通道粒度置 m_muted；
    // channel==TypeOfANone 时作用于全部内部音频源，保持接口语义不变。
    bool anyApplied = false;
    for (auto* src : m_audioSources) {
        if (!src) continue;
        if (channel == TypeOfANone || src->GetAudioCapSourceType() == channel) {
            src->SetMuted(mute);
            anyApplied = true;
        }
    }
    TG_LOGI("TGCapturer: MuteAudio mute=%d channel=%d applied=%d",
            mute ? 1 : 0, static_cast<int>(channel), anyApplied ? 1 : 0);
    return anyApplied;
}

bool TGCapturer::MuteVideo(bool mute)
{
    m_videoMuted.store(mute);
    return true;
}

int TGCapturer::SetPauseInfo(const PauseParams& params)
{
    // 对齐 MediaX ESGeneratePicture::SetPausePic：picture 为空 → 清缓存走冻结最后一帧语义；
    // 有路径 → 预解码到当前编码分辨率的 NV12，MuteVideo 期间由 OnVideoFrame 替换像素。
    m_pauseParams = params;
    if (params.picture.empty()) {
        if (m_pauseFrame) m_pauseFrame->Clear();
        TG_LOGI("TGCapturer: SetPauseInfo 清空（将冻结最后一帧）");
        return 0;
    }
    if (!m_pauseFrame) {
        m_pauseFrame = std::make_shared<core::PauseFrame>();
    }
    const int w = m_videoParams.width > 0 ? m_videoParams.width : 1280;
    const int h = m_videoParams.height > 0 ? m_videoParams.height : 720;
    if (!m_pauseFrame->Load(params.picture, w, h, params.rotate)) {
        TG_LOGE("TGCapturer: SetPauseInfo 加载失败 picture=%s", params.picture.c_str());
        return TGErr_InvalidArgument;
    }
    return 0;
}

int TGCapturer::SetupWatermarkInfo(const WatermarkParams& params)
{
    // 对齐 MediaX ESVPicFilter::AddOrRmLogo：支持 ADD / UPDATE / RM 多 index
    if (!m_watermark) m_watermark = std::make_shared<core::Watermark>();
    const int w = m_videoParams.width > 0 ? m_videoParams.width : 1280;
    const int h = m_videoParams.height > 0 ? m_videoParams.height : 720;
    const int ret = m_watermark->Apply(params, w, h);
    if (ret < 0) {
        TG_LOGE("TGCapturer: SetupWatermarkInfo 失败 index=%u ret=%d", params.index, ret);
    }
    return ret;
}

int TGCapturer::SetAVTimeout(int64_t audioTimeoutMs, int64_t videoTimeoutMs)
{
    // 两个阈值都用于断流看护（见 WatchLoop）：超过阈值没有数据就上报 TGErr_*CaptureStalled。
    // videoTimeoutMs 另外还是编码器侧"落后音频即丢帧"的兜底阈值。
    if (audioTimeoutMs <= 0 || videoTimeoutMs <= 0) {
        // 传 0 不能照单全收：maxLagUs=0 会让编码器把每一帧都判成"落后音频"而全部丢弃。
        // 要关闭看护请显式传负数（见下）
        TG_LOGE("TGCapturer: SetAVTimeout 入参无效 audio=%lld video=%lld（保留 %lld/%lld）",
                static_cast<long long>(audioTimeoutMs), static_cast<long long>(videoTimeoutMs),
                static_cast<long long>(m_audioTimeoutMs.load()),
                static_cast<long long>(m_videoTimeoutMs.load()));
        return TGErr_InvalidArgument;
    }
    m_audioTimeoutMs.store(audioTimeoutMs);
    m_videoTimeoutMs.store(videoTimeoutMs);
    m_videoEncoder->SetMaxLagUs(videoTimeoutMs * 1000);
    // 运行期调用也立即生效
    m_videoWatch.Configure(videoTimeoutMs * 1000);
    m_audioWatch.Configure(m_audioEnabled ? audioTimeoutMs * 1000 : 0);
    TG_LOGI("TGCapturer: SetAVTimeout audio=%lldms video=%lldms",
            static_cast<long long>(audioTimeoutMs), static_cast<long long>(videoTimeoutMs));
    return TGErr_OK;
}

// ------------------------------------------------------------------ 统计
// 全部走 JSON（简单 escape 不处理，key 为已知 ASCII，value 来自内部数值/枚举字符串）。
// 对齐 MediaX ESCapturer::GetStatsInfo：
//   bitrateInfo  → 下行 bitrate_bps + 累计字节/包
//   videoInfo    → encoder codec/encoded/dropped/pending/latency_us
//   audioInfo    → encoder codec/encoded/pending/latency_us + mixer 每路诊断
//   pktInfo      → muxer 底层 Stats（含 stall）
//   metaInfo/default → 汇总快照（video + audio + muxer + mixer）
std::string TGCapturer::GetStatsInfo(StatsInfoType type)
{
    auto mixerDiagJson = [this]() -> std::string {
        if (!m_mixer) return std::string("[]");
        auto diags = m_mixer->AllDiagnostics();
        std::string out = "[";
        for (size_t i = 0; i < diags.size(); ++i) {
            const auto& d = diags[i];
            char item[384];
            snprintf(item, sizeof(item),
                     "%s{\"underrun\":%llu,\"comp_us\":%lld,\"drop_us\":%lld,\"overflow_us\":%lld,"
                     "\"drift_us_per_5s\":%lld,\"ts_anomalies\":%llu,\"ts_fallback\":%s,"
                     "\"pi\":%llu,\"burst\":%llu,\"force\":%llu,\"drift_us\":%lld}",
                     i == 0 ? "" : ",",
                     static_cast<unsigned long long>(d.underrun),
                     static_cast<long long>(d.compensationUs),
                     static_cast<long long>(d.productionDropUs),
                     static_cast<long long>(d.overflowDropUs),
                     static_cast<long long>(d.residualDriftUs),
                     static_cast<unsigned long long>(d.tsAnomalies),
                     d.tsFallback ? "true" : "false",
                     static_cast<unsigned long long>(d.clock.piCount),
                     static_cast<unsigned long long>(d.clock.burstCount),
                     static_cast<unsigned long long>(d.clock.forceCount),
                     static_cast<long long>(d.clock.driftUs));
            out += item;
        }
        out += "]";
        return out;
    };

    // Muxer::Stats() 自带花括号，是完整 JSON 对象，直接嵌入，不能再包一层（否则输出 "{{...}}" 非法 JSON）
    const std::string muxerJson = m_muxer ? m_muxer->Stats() : std::string("{}");
    // 嵌入的变长段（mixer 每路诊断、muxer url）可能很长：按实际长度留够余量，避免截断成半个 JSON
    const std::string diag =
        (type == StatsInfoType::audioInfo || type == StatsInfoType::metaInfo) ? mixerDiagJson() : std::string();
    std::vector<char> buffer(1024 + muxerJson.size() + diag.size());
    switch (type) {
        case StatsInfoType::bitrateInfo:
            // 对齐 MediaX：bitrate + bytes + packets，统一报告"写出到网络/文件"的字节量
            snprintf(buffer.data(), buffer.size(),
                     "{\"bitrate_bps\":%.0f,\"bytes\":%llu,\"packets\":%llu}",
                     m_muxer ? m_muxer->TrafficBps() : 0.0f,
                     m_muxer ? static_cast<unsigned long long>(m_muxer->WrittenBytes()) : 0ULL,
                     m_muxer ? static_cast<unsigned long long>(m_muxer->WrittenPackets()) : 0ULL);
            break;
        case StatsInfoType::videoInfo:
            snprintf(buffer.data(), buffer.size(),
                     "{\"codec\":\"%s\",\"encoded\":%llu,\"dropped\":%llu,"
                     "\"pending\":%zu,\"latency_us\":%lld}",
                     m_videoEncoder ? m_videoEncoder->CodecName().c_str() : "",
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->EncodedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->DroppedFrames()) : 0ULL,
                     m_videoEncoder ? m_videoEncoder->Pending() : 0,
                     m_videoEncoder ? static_cast<long long>(m_videoEncoder->EncodeLatencyUs()) : 0LL);
            break;
        case StatsInfoType::audioInfo:
            // 音频：编码器基本信息 + mixer 每路 PI/BURST/补偿诊断
            snprintf(buffer.data(), buffer.size(),
                     "{\"codec\":\"%s\",\"encoded\":%llu,\"pending\":%zu,"
                     "\"latency_us\":%lld,\"mixer_channels\":%s}",
                     m_audioEncoder ? m_audioEncoder->CodecName().c_str() : "",
                     m_audioEncoder ? static_cast<unsigned long long>(m_audioEncoder->EncodedFrames()) : 0ULL,
                     m_audioEncoder ? m_audioEncoder->Pending() : 0,
                     m_audioEncoder ? static_cast<long long>(m_audioEncoder->EncodeLatencyUs()) : 0LL,
                     diag.c_str());
            break;
        case StatsInfoType::pktInfo:
            return muxerJson;
        case StatsInfoType::metaInfo:
        default:
            // 快照：三端通用的运行时概览
            snprintf(buffer.data(), buffer.size(),
                     "{\"bitrate_bps\":%.0f,\"video\":{\"codec\":\"%s\",\"encoded\":%llu,"
                     "\"dropped\":%llu,\"latency_us\":%lld},"
                     "\"audio\":{\"codec\":\"%s\",\"encoded\":%llu,\"latency_us\":%lld},"
                     "\"muxer\":%s,\"mixer\":%s}",
                     m_muxer ? m_muxer->TrafficBps() : 0.0f,
                     m_videoEncoder ? m_videoEncoder->CodecName().c_str() : "",
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->EncodedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->DroppedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<long long>(m_videoEncoder->EncodeLatencyUs()) : 0LL,
                     m_audioEncoder ? m_audioEncoder->CodecName().c_str() : "",
                     m_audioEncoder ? static_cast<unsigned long long>(m_audioEncoder->EncodedFrames()) : 0ULL,
                     m_audioEncoder ? static_cast<long long>(m_audioEncoder->EncodeLatencyUs()) : 0LL,
                     muxerJson.c_str(),
                     diag.c_str());
            break;
    }
    return std::string(buffer.data());
}

float TGCapturer::GetTrafficStats()
{
    return m_muxer ? m_muxer->TrafficBps() : 0.0f;
}

// ------------------------------------------------------------------ 回调
// 门面自己的错误 / 通知：异步发布，由总线的事件分发线程投递。
// 采集 / 编码线程调用这里不会被宿主回调阻塞
void TGCapturer::FireError(int32_t what, int32_t extra, const std::string& msg)
{
    m_bus.Publish(core::BusEventType::Error, "capturer", what, msg, extra);
}

void TGCapturer::FireNotify(NotifyMsgType type, const std::string& content, int64_t data)
{
    m_bus.Publish(core::BusEventType::Notify, "capturer", static_cast<int32_t>(type), content, 0, data);
}

// 总线事件的唯一出口：转给宿主回调（回调拷贝出来在锁外执行）。
// 内核自己要对事件做联动反应时（例如收到 X 就给某个节点发命令），也加在这里。
void TGCapturer::OnBusEvent(const core::BusEvent& event)
{
    switch (event.type) {
        case core::BusEventType::Error: {
            TG_LOGE("TGCapturer: error source=%s what=%d extra=%d msg=%s", event.source.c_str(),
                    event.code, event.extra, event.message.c_str());
            // 记下最后一个运行期错误码，Stop 时带给 OnStopCallback，宿主能据此判断本轮是否正常收尾。
            // 调用级误用（重复 Prepare、非法入参等）已经同步返回给调用方，且不影响本轮出流，
            // 不计入 —— 否则一次误调就让一轮完全正常的录制在 OnStop 里报错
            if (event.code != TGErr_InvalidState && event.code != TGErr_InvalidArgument &&
                event.code != TGErr_NotConfigured && event.code != TGErr_NotSupported) {
                m_lastRuntimeError.store(event.code);
            }
            OnErrorCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_onError;
            }
            if (cb) {
                cb(event.code, event.extra, event.message.c_str());
            }
            break;
        }
        case core::BusEventType::Notify: {
            OnNotifyCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_onNotify;
            }
            if (cb) {
                cb(static_cast<NotifyMsgType>(event.code), event.message, event.data);
            }
            break;
        }
        case core::BusEventType::Prepared: {
            OnPreparedCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_onPrepared;
            }
            if (cb) {
                cb(this);
            }
            break;
        }
        case core::BusEventType::Started: {
            OnStartCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_onStart;
            }
            if (cb) {
                cb();
            }
            break;
        }
        case core::BusEventType::Stopped: {
            OnStopCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_onStop;
            }
            if (cb) {
                cb(event.code);
            }
            break;
        }
    }
}

}  // namespace TGCapture
