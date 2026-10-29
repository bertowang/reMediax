#include "TGCapturer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "core/Logger.h"

// iOS 与 macOS 都定义 __APPLE__：原生采集（ReplayKit / AVAudioSession）只有 iOS 有，
// macOS 本机构建（单测 / 命令行 demo）和 Linux 一样只走外灌帧
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define TG_OS_IOS 1
#else
#define TG_OS_IOS 0
#endif

#ifdef _WIN32
#include "platform/windows/WinAudioCapture.h"
#include "platform/windows/WinScreenCapture.h"
#elif defined(__ANDROID__)
#include "platform/android/AndroidAudioCapture.h"
#include "platform/android/AndroidJni.h"
#include "platform/android/AndroidScreenCapture.h"
#elif TG_OS_IOS
#include "platform/ios/IOSAudioCapture.h"
#include "platform/ios/IOSScreenCapture.h"
#endif

// 有原生采集实现的平台。Linux 等平台没有（屏幕需 X11/PipeWire、音频需 PulseAudio/ALSA）：
// 采集接口一律返回不支持，但编码 / 封装 / 外灌帧不依赖它，故必须能编译通过。
// 没有这个宏时，下面那些 `#else return` 分支之后仍会引用未声明的 source，编译不过。
#if defined(_WIN32) || defined(__ANDROID__) || TG_OS_IOS
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
    m_videoEncoder = std::make_shared<codec::VideoEncoder>();
    m_audioEncoder = std::make_shared<codec::AudioEncoder>();
    m_muxer = std::make_shared<Muxer>();
    m_audioMixer = std::make_shared<nodes::AudioMixerNode>();
    m_vsrcMain = std::make_shared<nodes::VideoSourceNode>("vsrc.main");
    m_compositor = std::make_shared<nodes::VideoCompositorNode>();
    m_videoSync = std::make_shared<nodes::VideoSyncNode>();
    m_pauseFrame = std::make_shared<core::PauseFrame>();
    m_watermark = std::make_shared<core::Watermark>();

    m_videoEncoder->SetSyncClock(m_sync);
    m_audioEncoder->SetSyncClock(m_sync);

    // 共享服务经总线上下文注入（不是节点）：混音节点按输出推进音频主时钟，
    // video_sync 据音频主时钟反推视频 PTS
    m_bus.Context().Provide(m_audioClock);
    m_bus.Context().Provide(m_sync);
    m_bus.Context().Provide(m_videoPtsGen);
    m_bus.Context().Provide(m_videoOnlyTimeline);

    // 总线拓扑：
    //   asrc.* → audio_mixer → audio_encoder → muxer
    //   vsrc.main → video_compositor →(out0) video_sync → video_encoder → muxer
    // 数据、命令、事件都走这一条总线（见 core/Bus.h）。音频源节点在 AddAudioSource / 首次外灌时接入
    const bool linked = m_bus.Begin()
                            .Register("video_encoder", m_videoEncoder)
                            .Register("audio_encoder", m_audioEncoder)
                            .Register("muxer", m_muxer)
                            .Register("audio_mixer", m_audioMixer)
                            .Register("vsrc.main", m_vsrcMain)
                            .Register("video_compositor", m_compositor)
                            .Register("video_sync", m_videoSync)
                            .Link("video_encoder", "muxer")
                            .Link("audio_encoder", "muxer")
                            .Link("audio_mixer", "audio_encoder")
                            .Link("vsrc.main", 0, "video_compositor", nodes::VideoCompositorNode::kMainInput)
                            .Link("video_compositor", nodes::VideoCompositorNode::kProgramOutput, "video_sync", 0)
                            .Link("video_sync", "video_encoder")
                            .Commit();
    if (!linked) {
        TG_LOGE("TGCapturer: 总线拓扑建立失败");
    }
    // 暂停图 / 水印作为合成器的图层；主画面到达即为视频采集"活着"
    m_compositor->SetPauseLayer(m_pauseFrame);
    m_compositor->SetWatermarkLayer(m_watermark);
    m_compositor->SetMainActivityObserver([this](int64_t nowUs) { m_videoWatch.OnActivity(nowUs); });
    m_audioMixer->SetOutputObserver([this](int64_t nowUs) { m_audioWatch.OnActivity(nowUs); });
    m_audioMixer->SetHeartbeat([this] { LogHeartbeat(); });

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
#elif TG_OS_IOS
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
#elif TG_OS_IOS
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
    // AudioPlaybackCapture 与屏幕采集共用同一个 MediaProjection；不传的话 Open 直接返回 -1，
    // 表现为"内录开了但没声音"。因此宿主须先 SetPrivateData("android.MediaProjection") 再调本接口
    if (!m_mediaProjection) {
        TG_LOGW("TGCapturer: 系统内录需要先 SetPrivateData(\"android.MediaProjection\")，否则打开会失败");
    }
    source->SetProjection(static_cast<jobject>(m_mediaProjection));
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
    // m_mediaProjection 是 void*（宿主经 SetPrivateData 传入），JNI 侧要的是 jobject
    source->SetProjection(static_cast<jobject>(m_mediaProjection));
#elif TG_OS_IOS
    auto source = std::make_shared<platform::IOSScreenCapture>(strategy);
#endif

#if TG_HAS_NATIVE_CAPTURE
    m_ownedVideoSources.push_back(source);
    BindVideoSource(source.get());
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
    BindVideoSource(source.get());
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
    // 每路源一个 asrc.* 节点，接到混音器的空闲端口（端口号 = 混流通道号，由总线分配）
    const AudioCapSourceType type = base->GetAudioCapSourceType();
    const std::string name = UniqueAudioNodeName(type == TypeOfSelfMicphone ? "asrc.mic"
                                                 : type == TypeOfSpeaker    ? "asrc.speaker"
                                                                            : "asrc.src");
    auto node = std::make_shared<nodes::AudioSourceNode>(name, base);
    if (!m_bus.Begin().Register(name, node).Link(name, 0, "audio_mixer", core::kAnyPort).Commit()) {
        node->Detach();
        TG_LOGE("TGCapturer: 音频源 %s 接入总线失败", name.c_str());
        return TGErr_Internal;
    }
    m_audioSources.push_back(base);
    m_audioSourceNodes.push_back(std::move(node));
    if (!m_audioSource) {
        m_audioSource = base;
    }
    TG_LOGI("TGCapturer: 音频源 +1 %s（当前 %zu 路）", name.c_str(), m_audioSources.size());
    return 0;
}

std::string TGCapturer::UniqueAudioNodeName(const std::string& base) const
{
    if (!m_bus.Get(base)) {
        return base;
    }
    for (int i = 2;; ++i) {
        std::string name = base + "." + std::to_string(i);
        if (!m_bus.Get(name)) {
            return name;
        }
    }
}

void TGCapturer::RemoveExternalAudioNodes()
{
    std::unordered_map<int, std::shared_ptr<nodes::AudioSourceNode>> nodes;
    {
        std::lock_guard<std::mutex> lock(m_externalAudioMutex);
        nodes.swap(m_externalAudioNodes);
    }
    for (const auto& [_, node] : nodes) {
        m_bus.Unregister(node->Name());
    }
}

void TGCapturer::LogHeartbeat()
{
    // 1) 视频 PTS 生成器累计
    core::VideoPtsGenerator::Stats vs{};
    if (m_videoPtsGen) {
        vs = m_videoPtsGen->stats();
    }
    // 2) 每路音频的 drift / PI / BURST / ForceAlign / 积压
    const auto& mixer = m_audioMixer->Mixer();
    auto diags = mixer.AllDiagnostics();
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
                      mixer.BufferedFrames(static_cast<int>(i)));
        per += buf;
    }
    const auto ms = mixer.Stats();
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
            static_cast<unsigned long long>(m_videoSync->BackpressureDropped()),
            m_bus.TotalPending(),
            static_cast<unsigned long long>(m_bus.Events().Dropped()),
            static_cast<unsigned long long>(ms.outputFrames),
            static_cast<unsigned long long>(ms.waitTimeouts),
            static_cast<unsigned long long>(ms.reanchors),
            static_cast<long long>(ms.reanchorUs / 1000),
            per.c_str());
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
    BindVideoSource(base);
    return 0;
}

void TGCapturer::BindVideoSource(capture::VideoSourceBase* source)
{
    m_videoSource = source;
    m_vsrcMain->SetSource(source);
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
    // 上一轮外灌音频的 asrc.ext.* 节点（及其混流通道）作废，本轮按首帧格式重新接入
    RemoveExternalAudioNodes();

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
    const int64_t videoIntervalUs =
        m_videoParams.fps > 0 ? static_cast<int64_t>(1000000.0 / m_videoParams.fps) : 33333;
    nodes::VideoSyncNode::Config vsyncCfg;
    vsyncCfg.audioEnabled = m_audioEnabled;
    vsyncCfg.frameIntervalUs = videoIntervalUs;
    m_videoSync->SetConfig(vsyncCfg);
    m_videoSync->SetAudioTimeoutMs(m_audioTimeoutMs.load());
    // 计数、首帧标志（否则第二轮不再触发 EsCap_Notify_Fisrt_Frame_OK）、时间轴接续点
    m_videoSync->Reset();
    m_lastRuntimeError.store(0);
    // 断流看护的阈值在这里取用，Reset 留到 Start（Prepare 到 Start 之间的空档不算断流）
    m_videoWatch.Configure(m_videoTimeoutMs.load() * 1000);
    m_audioWatch.Configure(m_audioEnabled ? m_audioTimeoutMs.load() * 1000 : 0);
    // 编码器侧的"落后丢帧"只是最后一道兜底：阈值必须落在 VideoPtsGenerator 的
    // ForceAlign 区间之外（forceSyncFactor × 帧间隔 × 1.5）。
    // 否则会在 RangeAdapt 正常追赶期间（66~200ms @30fps）白白砍帧率，
    // 而时间轴并不受影响（PTS 由 Next() 独立推进），等于纯损失。
    const int64_t autoMaxLagUs = syncParams.forceSyncFactor * videoIntervalUs * 3 / 2;
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
    {
        // 硬编打不开时 VideoEncoder 已自动回落软编：告诉宿主实际用的是谁（否则只能翻日志）
        const std::string requested = m_videoParams.vCodec.empty() ? "libopenh264" : m_videoParams.vCodec;
        if (requested != m_videoEncoder->CodecName()) {
            FireNotify(EsCap_Notify_Encoder_Fallback,
                       "video:" + requested + "->" + m_videoEncoder->CodecName(), 0);
        }
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
        const std::string requested = m_audioParams.aCodec.empty() ? "libfdk_aac" : m_audioParams.aCodec;
        if (requested != m_audioEncoder->CodecName()) {
            FireNotify(EsCap_Notify_Encoder_Fallback,
                       "audio:" + requested + "->" + m_audioEncoder->CodecName(), 1);
        }
    } else {
        TG_LOGI("TGCapturer: 纯视频模式（audio disabled）");
    }

    if (m_muxer->Open(m_muxParams, m_videoEncoder->CodecContext(),
                      m_audioEnabled ? m_audioEncoder->CodecContext() : nullptr) < 0) {
        return fail(TGErr_OpenMuxer, m_muxer->LastAvError(), "open muxer failed");
    }

    // 音频链路：各路源节点先打开（失败按静音降级，见 AudioSourceNode::NodeConfigure），
    // 再配置混音节点 —— 输出格式取编码器配置，已接入的每路源预建一条通道（单路也同样建，以便做时钟校正）
    for (const auto& node : m_audioSourceNodes) {
        node->SetEnabled(m_audioEnabled);
        node->SetOpenFormat(m_audioParams.sampleRate, m_audioParams.channels);
    }
    m_audioMixer->SetEnabled(m_audioEnabled);
    if (m_audioEnabled) {
        for (const auto& node : m_audioSourceNodes) {
            node->Configure();
        }
        nodes::AudioMixerNode::Config mixCfg;
        mixCfg.sampleRate = m_audioParams.sampleRate;
        mixCfg.channels = m_audioParams.channels;
        mixCfg.frameSize = audioType.FrameSize() > 0 ? audioType.FrameSize() : 1024;
        mixCfg.clockCompensationUs = m_videoParams.audioCompensationUs;
        m_audioMixer->SetConfig(mixCfg);
        m_audioMixer->Configure();
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
    // output 组（封装器 / 编码器）由总线按拓扑顺序启动：下游先于上游（muxer → 编码器）；
    // 任一失败时总线已把本次起来的线程逆序停掉 —— 只退状态不停线程的话，muxer / 视频编码线程会继续空转，
    // 宿主若不 Stop 直接析构，它们就会在正在销毁的成员上继续发包。
    // 纯视频模式下音频编码器未打开（IsActive=false），不会被启动。
    const auto started = m_bus.StartGroup(core::NodeGroup::Output);
    if (!started.ok) {
        const int32_t stage = started.failedNode == "muxer"           ? 1
                              : started.failedNode == "video_encoder" ? 2
                              : started.failedNode == "audio_encoder" ? 3
                                                                      : 0;
        TG_LOGE("TGCapturer: %s 线程启动失败（上一轮可能未停止干净）", started.failedNode.c_str());
        err = {TGErr_StartFailed, stage, "start failed: " + started.failedNode};
        m_state.store(State::Prepared);   // 退回可重试的状态，宿主可以 Stop 后重开
        return static_cast<int>(TGErr_StartFailed);
    }
    // capture 组（vsrc.main / 合成器 / 音频源 / 混音器）：下游先于上游 —— 合成器、混音时钟线程先起，
    // 源再开始产帧（视频源节点在这里 StartCapture）。纯视频模式下音频节点未激活，被跳过
    m_bus.StartGroup(core::NodeGroup::Capture);

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
    // capture 组（源 → 合成器 / 混音器）：上游先于下游。源节点是受控输出（关闸 + 停源 + 等在飞回调；
    // 视频源节点在这里 StopCapture —— 它是真正的回调栅栏，返回后拆编码器 / 混流器时不会再有采集线程闯进来），
    // 混音器停线程并复位音频主时钟。不按 m_audioEnabled 过滤：Prepare 中途失败时它可能还是上一轮的值，
    // 而已经 Open 过的源无论如何都得停。对没启动的源是无副作用的空操作。
    m_bus.StopGroup(core::NodeGroup::Capture);
    // output 组（video_sync / 编码器 / 封装器）由总线按拓扑顺序收尾：Stop 上游先于下游（编码器 drain 完积压再停封装器），
    // Close 下游先于上游（封装器先写尾，再关编码器）。对未启动 / 未打开的节点是空操作
    m_bus.StopGroup(core::NodeGroup::Output);
    m_bus.CloseGroup(core::NodeGroup::Output);
    // 采集源的设备资源（摄像头/麦克风句柄、COM 对象、D3D 纹理、GDI 位图）最后放（vsrc.main 的 NodeClose）。
    // 此时采集线程已 join、在飞回调已退出，Close 不会与产帧并发。
    m_bus.CloseGroup(core::NodeGroup::Capture);
    RemoveExternalAudioNodes();
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
                static_cast<unsigned long long>(m_videoSync->SyncDropped()),
                static_cast<unsigned long long>(m_videoSync->BackpressureDropped()));
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
    // 先把音频源节点从总线上摘下并解除与源的回调绑定，再释放源（源可能比节点先销毁）
    for (const auto& node : m_audioSourceNodes) {
        node->Detach();
        m_bus.Unregister(node->Name());
    }
    m_audioSourceNodes.clear();
    RemoveExternalAudioNodes();
    // 视频源同理：先解除 vsrc.main 与源的回调绑定，再释放源
    BindVideoSource(nullptr);
    m_ownedVideoSources.clear();
    m_ownedAudioSources.clear();
    m_audioSource = nullptr;
    m_audioSources.clear();
    if (m_audioClock) {
        m_audioClock->Reset();
    }
    m_audioMixer->Mixer().Reset();
}

// ------------------------------------------------------------------ 帧流转
// 外部注入的音频：每种 AudioCapSourceType 一个 asrc.ext.<type> 节点，首帧到达时接入混音器
void TGCapturer::OnAudioFrame(AudioCapSourceType type, core::FramePtr frame)
{
    if (!frame || !m_audioEnabled || m_state.load() != State::Running) {
        return;
    }
    if (!frame->audioData || frame->audioBytes == 0 || frame->channels <= 0) {
        return;
    }
    // 外部注入的音频按 type 分路：每种 AudioCapSourceType 独占一个混流通道。
    // 共享单一通道会把两路串成一条时间轴（表现为"交替输出"而非真正混音），且与内部麦克风源
    // 共用同一个通道时钟还会互相拉扯时间戳（音频抖动/变调）。
    std::shared_ptr<nodes::AudioSourceNode> node;
    {
        std::lock_guard<std::mutex> lock(m_externalAudioMutex);
        const int key = static_cast<int>(type);
        auto it = m_externalAudioNodes.find(key);
        if (it != m_externalAudioNodes.end()) {
            node = it->second;
        } else {
            const std::string name = UniqueAudioNodeName("asrc.ext." + std::to_string(key));
            node = std::make_shared<nodes::AudioSourceNode>(name, type);
            if (!m_bus.Begin().Register(name, node).Link(name, 0, "audio_mixer", core::kAnyPort).Commit()) {
                // 接不进去 = 这一路音频被永久丢弃，原先静默 return，宿主完全无感
                TG_LOGE("TGCapturer: 外部注入音频 type=%d 接入总线失败", key);
                FireError(TGErr_Internal, 0, "add audio mixer channel failed");
                return;
            }
            m_externalAudioNodes[key] = node;
            TG_LOGI("TGCapturer: 外部注入音频 type=%d 接入 %s（%dHz %dch）", key, name.c_str(),
                    frame->sampleRate, frame->channels);
        }
    }
    node->Push(std::move(frame));
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
    // 与原生采集同一入口：vsrc.main → video_compositor → video_sync → video_encoder
    m_vsrcMain->Push(std::move(frame));
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
    m_compositor->SetMuted(mute);
    return true;
}

int TGCapturer::SetPauseInfo(const PauseParams& params)
{
    // 对齐 MediaX ESGeneratePicture::SetPausePic：picture 为空 → 清缓存走冻结最后一帧语义；
    // 有路径 → 预解码到当前编码分辨率的 NV12，MuteVideo 期间由合成器的暂停图层替换像素。
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
    m_videoSync->SetAudioTimeoutMs(audioTimeoutMs);
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
//   bitrateInfo  → 实时码率（总 / 音视频分路 / 封装后）+ 累计字节（同口径）/ 包数 / 平均码率 / 时长
//   videoInfo    → encoder codec/encoded/dropped/pending/latency_us
//   audioInfo    → encoder codec/encoded/pending/latency_us + mixer 每路诊断
//   pktInfo      → muxer 底层 Stats（含 stall）
//   metaInfo/default → 汇总快照（video + audio + muxer + mixer）
std::string TGCapturer::GetStatsInfo(StatsInfoType type)
{
    auto mixerDiagJson = [this]() -> std::string {
        auto diags = m_audioMixer->Mixer().AllDiagnostics();
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
        case StatsInfoType::bitrateInfo: {
            // 实时 + 累计两种口径（字段说明见 ITGCapturer.h GetStatsInfo）。
            // 兼容：bitrate_bps / packets 含义不变；bytes 此前误为"1s 窗口内字节数"（每秒清零），现为真正的累计值
            const Muxer::TrafficSnapshot t = m_muxer ? m_muxer->Traffic() : Muxer::TrafficSnapshot{};
            snprintf(buffer.data(), buffer.size(),
                     "{\"bitrate_bps\":%llu,\"video_bps\":%llu,\"audio_bps\":%llu,\"io_bps\":%llu,"
                     "\"bytes\":%llu,\"video_bytes\":%llu,\"audio_bytes\":%llu,\"io_bytes\":%llu,"
                     "\"packets\":%llu,\"avg_bps\":%llu,\"duration_ms\":%lld}",
                     static_cast<unsigned long long>(t.bps), static_cast<unsigned long long>(t.videoBps),
                     static_cast<unsigned long long>(t.audioBps), static_cast<unsigned long long>(t.ioBps),
                     static_cast<unsigned long long>(t.bytes), static_cast<unsigned long long>(t.videoBytes),
                     static_cast<unsigned long long>(t.audioBytes), static_cast<unsigned long long>(t.ioBytes),
                     static_cast<unsigned long long>(t.packets), static_cast<unsigned long long>(t.avgBps),
                     static_cast<long long>(t.durationUs / 1000));
            break;
        }
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
