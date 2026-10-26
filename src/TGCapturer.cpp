#include "TGCapturer.h"

#include <algorithm>
#include <chrono>

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

    // 总线：采集 -> 编码器 -> 封装
    m_bus.Register("video_encoder", m_videoEncoder);
    m_bus.Register("audio_encoder", m_audioEncoder);
    m_videoEncoder->LinkNext(nullptr);  // 编码器直接投递给 muxer
    m_audioEncoder->LinkNext(nullptr);
}

TGCapturer::~TGCapturer()
{
    Stop();
    ReleaseAll();
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
        return -1;
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
    return -1;
#endif
}

int TGCapturer::EnableSpeakerCapture()
{
    if (!IsSupported(CaptureFeature::Speaker)) {
        TG_LOGE("TGCapturer: 当前平台不支持扬声器/系统内录");
        return -1;
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
    return -1;
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
        return -1;
    }
    auto* base = dynamic_cast<capture::AudioSourceBase*>(source);
    if (!base) {
        return -2;
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
        m_mixer->Reset();
    }
}

void TGCapturer::MixLoop(std::stop_token token)
{
    // 心跳：每 10 秒打一条累计状态，长压测时能在时间轴上看到 drift / avDiff 的分布，
    // 而不是只在 Stop 时才有一次最终聚合
    const int64_t kHeartbeatUs = 10 * 1000000LL;
    int64_t lastBeatUs = core::MonotonicClock::NowUs();
    while (!token.stop_requested()) {
        std::shared_ptr<uint8_t> data;
        int64_t ptsUs = 0;
        if (!m_mixer->Mix(m_audioFrameSize, data, ptsUs)) {
            // 所有混流通道都没凑够一帧数据：限频提示，避免"音频时间轴悄悄停住"却没有任何迹象。
            // "通道数" 指当前挂载的混流通道总数，不是通道号 —— 之前文案 "通道 N" 容易被误读
            TG_LOGW_THROTTLED(3000000, "TGCapturer: 混流取不到数据（当前通道数 %zu，帧长 %d）",
                              m_mixer->ChannelCount(), m_audioFrameSize);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

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
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              " ch%zu[drift=%lldms pi=%llu burst=%llu force=%llu "
                              "jump=%llu under=%llu comp=%lldms drop=%lldms buf=%zu]",
                              i,
                              static_cast<long long>(d.clock.driftUs / 1000),
                              static_cast<unsigned long long>(d.clock.piCount),
                              static_cast<unsigned long long>(d.clock.burstCount),
                              static_cast<unsigned long long>(d.clock.forceCount),
                              static_cast<unsigned long long>(d.clock.jumpCount),
                              static_cast<unsigned long long>(d.underrun),
                              static_cast<long long>(d.compensationUs / 1000),
                              static_cast<long long>(d.productionDropUs / 1000),
                              m_mixer->BufferedFrames(static_cast<int>(i)));
                per += buf;
            }
            TG_LOGI("TGCapturer: 心跳 avDiff=%lldms force=%llu adapt=%llu clamp=%llu "
                    "stall=%llu skip=%llu jump=%llu staleReset=%llu|%s",
                    static_cast<long long>(vs.avDiffUs / 1000),
                    static_cast<unsigned long long>(vs.forceSynced),
                    static_cast<unsigned long long>(vs.adapted),
                    static_cast<unsigned long long>(vs.clamped),
                    static_cast<unsigned long long>(vs.stalled),
                    static_cast<unsigned long long>(vs.skipped),
                    static_cast<unsigned long long>(vs.jumpWarns),
                    static_cast<unsigned long long>(vs.staleResets),
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
        m_bus.Dispatch("audio_encoder", std::move(frame));
    }
}

int TGCapturer::AddVideoSource(IVideoSource* source)
{
    if (!source) {
        return -1;
    }
    auto* base = dynamic_cast<capture::VideoSourceBase*>(source);
    if (!base) {
        return -2;
    }
    m_videoSource = base;
    base->SetEmitCallback([this](core::FramePtr frame) { OnVideoFrame(std::move(frame)); });
    return 0;
}

// ------------------------------------------------------------------ 参数
int TGCapturer::SetupAudioEncoder(const AudioEncoderParams& params)
{
    m_audioParams = params;
    m_audioParamsSet = true;
    return 0;
}

int TGCapturer::SetupVideoEncoder(const VideoEncoderParams& params)
{
    m_videoParams = params;
    m_videoParamsSet = true;
    m_perfMode = params.perfMode;
    return 0;
}

int TGCapturer::SetupMuxer(const MuxerParams& params)
{
    m_muxParams = params;
    m_muxParamsSet = true;
    return 0;
}

void TGCapturer::SetOnPreparedCallback(OnPreparedCallback cb) { m_onPrepared = std::move(cb); }
void TGCapturer::SetOnErrorCallback(OnErrorCallback cb) { m_onError = std::move(cb); }
void TGCapturer::SetOnStartCallback(OnStartCallback cb) { m_onStart = std::move(cb); }
void TGCapturer::SetOnStopCallback(OnStopCallback cb) { m_onStop = std::move(cb); }
void TGCapturer::SetOnNotifyCallback(OnNotifyCallback cb) { m_onNotify = std::move(cb); }

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
int TGCapturer::Prepare()
{
    std::lock_guard<std::recursive_mutex> lock(m_lifecycleMutex);
    // 检查与置位在同一个临界区内原子完成：并发两次 Prepare 只有一个能进
    State expected = State::Idle;
    if (!m_state.compare_exchange_strong(expected, State::Preparing)) {
        TG_LOGW("TGCapturer: Prepare 状态错误 state=%d", static_cast<int>(m_state.load()));
        return -1;
    }

    if (!m_videoParamsSet || !m_audioParamsSet || !m_muxParamsSet) {
        FireError(-1, 0, "encoders or muxer not configured");
        m_state.store(State::Idle);
        return -2;
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
    if (m_videoSource
        && m_videoSource->Open(m_videoParams.width, m_videoParams.height, m_videoParams.fps) < 0) {
        TG_LOGW("TGCapturer: 视频源打开失败（外部注入模式可忽略）");
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
    m_syncDropped = 0;
    m_audioWaitStartUs = 0;
    // 首帧标志必须复位，否则第二轮不再触发 EsCap_Notify_Fisrt_Frame_OK
    m_firstFrameSent.store(false);
    m_lastVideoUs = 0;
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
        FireError(-3, 0, "open video encoder failed");
        m_state.store(State::Idle);
        return -3;
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
            FireError(-4, 0, "open audio encoder failed");
            m_state.store(State::Idle);
            return -4;
        }
    } else {
        TG_LOGI("TGCapturer: 纯视频模式（audio disabled）");
    }

    m_videoEncoder->SetMuxer(m_muxer.get());
    m_audioEncoder->SetMuxer(m_muxer.get());

    if (m_muxer->Open(m_muxParams, m_videoEncoder->CodecContext(),
                      m_audioEnabled ? m_audioEncoder->CodecContext() : nullptr) < 0) {
        FireError(-5, 0, "open muxer failed");
        m_state.store(State::Idle);
        return -5;
    }

    if (m_audioEnabled) {
        for (auto* source : m_audioSources) {
            if (source && source->Open(m_audioParams.sampleRate, m_audioParams.channels) < 0) {
                TG_LOGW("TGCapturer: 音频源打开失败（该路按静音处理）");
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
    if (m_onPrepared) {
        m_onPrepared(this);
    }
    return 0;
}

int TGCapturer::Start()
{
    std::lock_guard<std::recursive_mutex> lock(m_lifecycleMutex);
    State expected = State::Prepared;
    if (!m_state.compare_exchange_strong(expected, State::Starting)) {
        TG_LOGW("TGCapturer: Start 状态错误 state=%d", static_cast<int>(m_state.load()));
        return -1;
    }

    if (!m_muxer->Start()) {
        TG_LOGW("TGCapturer: muxer 线程启动异常（可能上一轮未停止干净）");
    }
    if (!m_videoEncoder->Start()) {
        TG_LOGW("TGCapturer: 视频编码线程启动异常（可能上一轮未停止干净）");
    }
    if (m_audioEnabled) {
        m_audioEncoder->Start();
    }
    if (m_videoSource) {
        m_videoSource->Start();
    }
    if (m_audioEnabled) {
        for (auto* source : m_audioSources) {
            if (source) {
                source->Start();
            }
        }
        StartAudioMixer();   // 多于一路时合并（麦克风 + 扬声器）
    }

    m_state.store(State::Running);
    if (m_onStart) {
        m_onStart();
    }
    TG_LOGI("TGCapturer: 已启动");
    return 0;
}

void TGCapturer::Stop()
{
    // 与 Prepare / Start 共用同一把锁：三者不会交错执行，
    // 从根上消除"Stop 跑完时 Start 还没启动线程"造成的野线程。
    std::lock_guard<std::recursive_mutex> lock(m_lifecycleMutex);
    const State cur = m_state.load();
    if (cur == State::Idle || cur == State::Stopping) {
        return;
    }
    m_state.store(State::Stopping);

    // 按数据流向逆序停止：先停采集，再 drain 编码器，最后收尾封装
    if (m_videoSource) {
        m_videoSource->StopCapture();
    }
    StopAudioMixer();
    if (m_audioEnabled) {
        for (auto* source : m_audioSources) {
            if (source) {
                source->StopCapture();
            }
        }
    }
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

    m_sync->Reset();
    if (m_videoPtsGen) {
        const auto st = m_videoPtsGen->stats();
        TG_LOGI("TGCapturer: 音画同步 avDiff=%lldms clamp=%llu forceSync=%llu adapt=%llu stall=%llu skip=%llu jump=%llu staleReset=%llu syncDrop=%llu",
                static_cast<long long>(st.avDiffUs / 1000),
                static_cast<unsigned long long>(st.clamped),
                static_cast<unsigned long long>(st.forceSynced),
                static_cast<unsigned long long>(st.adapted),
                static_cast<unsigned long long>(st.stalled),
                static_cast<unsigned long long>(st.skipped),
                static_cast<unsigned long long>(st.jumpWarns),
                static_cast<unsigned long long>(st.staleResets),
                static_cast<unsigned long long>(m_syncDropped));
    }
    m_state.store(State::Idle);
    if (m_onStop) {
        m_onStop(0);
    }
    TG_LOGI("TGCapturer: 已停止");
}

void TGCapturer::ReleaseAll()
{
    std::lock_guard<std::recursive_mutex> lock(m_lifecycleMutex);
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
    if (m_videoEncoder->Pending() >= kMaxEncoderPending) {
        TG_LOGW_THROTTLED(2000000, "TGCapturer: 编码器积压 %zu，丢帧（编码跟不上采集）",
                          m_videoEncoder->Pending());
        return;
    }

    // 调用方自带时间轴：直接用它给的 PTS，不再走内核的同步与时间轴生成
    if (frame->ptsLocked && frame->ptsUs >= 0) {
        const int frameW = frame->width;
        const int frameH = frame->height;
        const int64_t framePts = frame->ptsUs;
        if (!m_bus.Dispatch("video_encoder", std::move(frame))) {
            return;
        }
        if (!m_firstFrameSent.exchange(true)) {
            TG_LOGI("TGCapturer: 首帧视频 %dx%d pts=%lld（外部时间轴）", frameW, frameH,
                    static_cast<long long>(framePts));
            FireNotify(EsCap_Notify_Fisrt_Frame_OK, "first frame", 0);
        }
        m_lastVideoUs = core::MonotonicClock::NowUs();
        return;
    }

    // ---- 音画同步：视频 PTS 由音频时钟反推（不用采集 wall clock）----
    // 对齐 MediaX：每帧独立判定，不保留"已降级"状态 ——
    // 音频恢复后下一帧 m_audioClock->IsFresh() 自然为 true，直接切回主时钟，
    // VideoOnlyTimeline 也顺手 Reset 让再次掉线能重新从 0 起算。
    //
    // 关键：判活用 IsFresh(audioTimeoutMs) 而不是 Valid()。Valid() 是单向开关
    // （第一次 Advance/Reserve 后永远 true），音频长时间断流时 ReferencePtsUs()
    // 会一直返回最后一次推进的旧 PTS —— VideoPtsGenerator 的 stallFrames 虽然能
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
        // 音频刚从异常恢复：重置等待起点 + 让 videoOnlyTimeline 下次能从 0 重起
        if (m_audioWaitStartUs != 0) {
            TG_LOGI("TGCapturer: 音频主时钟恢复，切回主时钟（曾等待/断流 %lldms）",
                    static_cast<long long>((core::MonotonicClock::NowUs() - m_audioWaitStartUs) / 1000));
            m_audioWaitStartUs = 0;
            m_videoOnlyTimeline->Reset();
        }
    } else if (m_audioEnabled) {
        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (m_audioWaitStartUs == 0) {
            m_audioWaitStartUs = nowUs;
            // 区分"首帧前等待" vs "运行中断流"的日志，便于定位
            if (m_audioClock && m_audioClock->Valid()) {
                TG_LOGW("TGCapturer: 音频主时钟断流（最后一拍 %lldms 前），进入 wall-clock fallback",
                        static_cast<long long>(m_audioTimeoutMs));
            } else {
                TG_LOGI("TGCapturer: 等待音频主时钟首帧（最多 %lldms）",
                        static_cast<long long>(m_audioTimeoutMs));
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
        const int64_t pts = m_videoPtsGen->Next(audioRef, m_videoIntervalUs, true);
        if (pts == core::kInvalidPtsUs) {
            ++m_syncDropped;   // 音频停滞且已 clamp 到上限：本帧不编码
            TG_LOGW_THROTTLED(2000000, "TGCapturer: 音频停滞且视频已到超前上限，丢帧（syncDrop=%llu）",
                              static_cast<unsigned long long>(m_syncDropped));
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

    if (!m_bus.Dispatch("video_encoder", std::move(frame))) {
        return;
    }

    if (!m_firstFrameSent.exchange(true)) {
        TG_LOGI("TGCapturer: 首帧视频 %dx%d pts=%lld", width, height, static_cast<long long>(pts));
        FireNotify(EsCap_Notify_Fisrt_Frame_OK, "first frame", 0);
    }
    m_lastVideoUs = core::MonotonicClock::NowUs();
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
        return -1;
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
        return -4;
#endif
        // 硬件帧没有系统内存像素，pixFmt 无意义（与采集侧 GPU 帧保持一致）
        frame->pixFmt = AV_PIX_FMT_NONE;
    } else if (data) {
        if (width <= 0 || height <= 0) {
            return -3;
        }
        const int rowStride = stride > 0 ? stride : width;
        if (rowStride < width) {
            return -3;   // stride 不能小于宽度，否则 Y 平面行会重叠
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
        return -2;
    }
    // 调用方自带时间轴时锁定 PTS，内核不再用音频主时钟改写（见 videoUseExternalPts）
    frame->ptsLocked = m_useExternalVideoPts && ptsUs >= 0;
    OnVideoFrame(std::move(frame));
    return 0;
}

int TGCapturer::PushExternalAudioFrame(AudioCapSourceType type, const uint8_t* data, int size,
                                       int sampleRate, int channels, int64_t ptsUs)
{
    if (m_state.load() != State::Running || !data || size <= 0) {
        return -1;
    }
    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Audio;
    frame->sampleRate = sampleRate;
    frame->channels = channels;
    frame->sampleFmt = AV_SAMPLE_FMT_S16;
    frame->audioBytes = static_cast<size_t>(size);
    frame->audioData = std::shared_ptr<uint8_t>(new uint8_t[static_cast<size_t>(size)], std::default_delete<uint8_t[]>());
    memcpy(frame->audioData.get(), data, static_cast<size_t>(size));
    frame->ptsUs = ptsUs >= 0 ? ptsUs : core::MonotonicClock::NowUs();
    OnAudioFrame(type, std::move(frame));
    return 0;
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
        return -1;
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
    if (audioTimeoutMs > 0) {
        m_audioTimeoutMs = audioTimeoutMs;
    }
    if (videoTimeoutMs > 0) {
        m_videoTimeoutMs = videoTimeoutMs;
        m_videoEncoder->SetMaxLagUs(videoTimeoutMs * 1000);
    } else {
        // 传 0 不能照单全收：maxLagUs=0 会让编码器把每一帧都判成"落后音频"而全部丢弃
        TG_LOGW("TGCapturer: SetAVTimeout 的 videoTimeoutMs=%lld 无效，保留 %lldms",
                static_cast<long long>(videoTimeoutMs), static_cast<long long>(m_videoTimeoutMs));
    }
    return 0;
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
            char item[320];
            snprintf(item, sizeof(item),
                     "%s{\"underrun\":%llu,\"comp_us\":%lld,\"drop_us\":%lld,"
                     "\"pi\":%llu,\"burst\":%llu,\"force\":%llu,\"drift_us\":%lld}",
                     i == 0 ? "" : ",",
                     static_cast<unsigned long long>(d.underrun),
                     static_cast<long long>(d.compensationUs),
                     static_cast<long long>(d.productionDropUs),
                     static_cast<unsigned long long>(d.clock.piCount),
                     static_cast<unsigned long long>(d.clock.burstCount),
                     static_cast<unsigned long long>(d.clock.forceCount),
                     static_cast<long long>(d.clock.driftUs));
            out += item;
        }
        out += "]";
        return out;
    };

    char buffer[1024];
    switch (type) {
        case StatsInfoType::bitrateInfo:
            // 对齐 MediaX：bitrate + bytes + packets，统一报告"写出到网络/文件"的字节量
            snprintf(buffer, sizeof(buffer),
                     "{\"bitrate_bps\":%.0f,\"bytes\":%llu,\"packets\":%llu}",
                     m_muxer ? m_muxer->TrafficBps() : 0.0f,
                     m_muxer ? static_cast<unsigned long long>(m_muxer->WrittenBytes()) : 0ULL,
                     m_muxer ? static_cast<unsigned long long>(m_muxer->WrittenPackets()) : 0ULL);
            break;
        case StatsInfoType::videoInfo:
            snprintf(buffer, sizeof(buffer),
                     "{\"codec\":\"%s\",\"encoded\":%llu,\"dropped\":%llu,"
                     "\"pending\":%zu,\"latency_us\":%lld}",
                     m_videoEncoder ? m_videoEncoder->CodecName().c_str() : "",
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->EncodedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->DroppedFrames()) : 0ULL,
                     m_videoEncoder ? m_videoEncoder->Pending() : 0,
                     m_videoEncoder ? static_cast<long long>(m_videoEncoder->EncodeLatencyUs()) : 0LL);
            break;
        case StatsInfoType::audioInfo: {
            // 音频：编码器基本信息 + mixer 每路 PI/BURST/补偿诊断
            const std::string diag = mixerDiagJson();
            snprintf(buffer, sizeof(buffer),
                     "{\"codec\":\"%s\",\"encoded\":%llu,\"pending\":%zu,"
                     "\"latency_us\":%lld,\"mixer_channels\":%s}",
                     m_audioEncoder ? m_audioEncoder->CodecName().c_str() : "",
                     m_audioEncoder ? static_cast<unsigned long long>(m_audioEncoder->EncodedFrames()) : 0ULL,
                     m_audioEncoder ? m_audioEncoder->Pending() : 0,
                     m_audioEncoder ? static_cast<long long>(m_audioEncoder->EncodeLatencyUs()) : 0LL,
                     diag.c_str());
            break;
        }
        case StatsInfoType::pktInfo:
            snprintf(buffer, sizeof(buffer), "{%s}", m_muxer ? m_muxer->Stats().c_str() : "");
            break;
        case StatsInfoType::metaInfo:
        default: {
            // 快照：三端通用的运行时概览
            const std::string diag = mixerDiagJson();
            snprintf(buffer, sizeof(buffer),
                     "{\"bitrate_bps\":%.0f,\"video\":{\"codec\":\"%s\",\"encoded\":%llu,"
                     "\"dropped\":%llu,\"latency_us\":%lld},"
                     "\"audio\":{\"codec\":\"%s\",\"encoded\":%llu,\"latency_us\":%lld},"
                     "\"muxer\":{%s},\"mixer\":%s}",
                     m_muxer ? m_muxer->TrafficBps() : 0.0f,
                     m_videoEncoder ? m_videoEncoder->CodecName().c_str() : "",
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->EncodedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->DroppedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<long long>(m_videoEncoder->EncodeLatencyUs()) : 0LL,
                     m_audioEncoder ? m_audioEncoder->CodecName().c_str() : "",
                     m_audioEncoder ? static_cast<unsigned long long>(m_audioEncoder->EncodedFrames()) : 0ULL,
                     m_audioEncoder ? static_cast<long long>(m_audioEncoder->EncodeLatencyUs()) : 0LL,
                     m_muxer ? m_muxer->Stats().c_str() : "",
                     diag.c_str());
            break;
        }
    }
    return std::string(buffer);
}

float TGCapturer::GetTrafficStats()
{
    return m_muxer ? m_muxer->TrafficBps() : 0.0f;
}

// ------------------------------------------------------------------ 回调
void TGCapturer::FireError(int32_t what, int32_t extra, const std::string& msg)
{
    TG_LOGE("TGCapturer: error what=%d extra=%d msg=%s", what, extra, msg.c_str());
    if (m_onError) {
        m_onError(what, extra, msg.c_str());
    }
}

void TGCapturer::FireNotify(NotifyMsgType type, const std::string& content, int64_t data)
{
    if (m_onNotify) {
        m_onNotify(type, content, data);
    }
}

}  // namespace TGCapture
