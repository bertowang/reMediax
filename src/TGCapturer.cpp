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

namespace TGCapture
{

TGCapturer::TGCapturer(GameID gameId) : m_gameId(gameId)
{
    m_sync = std::make_shared<core::AVSync>();
    m_videoEncoder = std::make_shared<codec::VideoEncoder>();
    m_audioEncoder = std::make_shared<codec::AudioEncoder>();
    m_muxer = std::make_shared<Muxer>();

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
#else
    return -1;
#endif
    m_ownedAudioSources.push_back(source);
    return AddAudioSource(source.get());
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
#else
    return -1;
#endif
    m_ownedAudioSources.push_back(source);
    return AddAudioSource(source.get());
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
#else
    return nullptr;
#endif
    m_ownedVideoSources.push_back(source);
    m_videoSource = source.get();
    return source.get();
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

void TGCapturer::OnAudioFrameFrom(capture::AudioSourceBase* source, core::FramePtr frame)
{
    if (!frame || !m_audioEnabled || m_state.load() != State::Running) {
        return;
    }
    if (m_audioSources.size() <= 1) {
        OnAudioFrame(std::move(frame));   // 单路：直接透传，零额外延迟
        return;
    }
    // 多路：先缓存各源的最新帧，由混音线程合并
    if (frame->ptsUs < 0) {
        frame->ptsUs = frame->captureUs;
    }
    std::lock_guard<std::mutex> lock(m_audioMixMutex);
    m_audioLatest[source] = std::move(frame);
}

void TGCapturer::StartAudioMixer()
{
    if (m_audioSources.size() <= 1 || m_mixThread.joinable()) {
        return;
    }
    TG_LOGI("TGCapturer: 启用 %zu 路音频混音", m_audioSources.size());
    m_mixThread = std::jthread([this](std::stop_token st) { MixLoop(st); });
}

void TGCapturer::StopAudioMixer()
{
    if (m_mixThread.joinable()) {
        m_mixThread.request_stop();
        m_mixThread.join();
    }
    std::lock_guard<std::mutex> lock(m_audioMixMutex);
    m_audioLatest.clear();
}

void TGCapturer::MixLoop(std::stop_token token)
{
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        std::vector<core::FramePtr> batch;
        {
            std::lock_guard<std::mutex> lock(m_audioMixMutex);
            if (m_audioLatest.empty()) {
                continue;
            }
            batch.reserve(m_audioLatest.size());
            for (auto& item : m_audioLatest) {
                if (item.second) {
                    batch.push_back(std::move(item.second));
                }
            }
            m_audioLatest.clear();
        }
        if (batch.empty()) {
            continue;
        }

        // 以最短的一帧为准做加和混音（S16 交错，饱和截断）
        const core::FramePtr& first = batch[0];
        size_t minSamples = first->audioBytes / sizeof(int16_t);
        for (const auto& f : batch) {
            minSamples = std::min(minSamples, f->audioBytes / sizeof(int16_t));
        }
        if (minSamples == 0) {
            continue;
        }
        const size_t bytes = minSamples * sizeof(int16_t);
        auto mixed = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());

        std::vector<int32_t> acc(minSamples, 0);   // 中间按 32 位累加，避免溢出
        for (const auto& f : batch) {
            const auto* src = reinterpret_cast<const int16_t*>(f->audioData.get());
            for (size_t i = 0; i < minSamples; ++i) {
                acc[i] += src[i];
            }
        }
        auto* out = reinterpret_cast<int16_t*>(mixed.get());
        for (size_t i = 0; i < minSamples; ++i) {
            out[i] = static_cast<int16_t>(std::clamp(acc[i], -32768, 32767));
        }

        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Audio;
        frame->sampleRate = first->sampleRate;
        frame->channels = first->channels;
        frame->sampleFmt = AV_SAMPLE_FMT_S16;
        frame->planar = false;
        frame->nbSamples = static_cast<int>(minSamples / static_cast<size_t>(first->channels > 0 ? first->channels : 1));
        frame->audioData = mixed;
        frame->audioBytes = bytes;
        frame->ptsUs = first->ptsUs;
        frame->captureUs = core::MonotonicClock::NowUs();
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
    if (m_state.load() != State::Idle) {
        TG_LOGW("TGCapturer: Prepare 状态错误 state=%d", static_cast<int>(m_state.load()));
        return -1;
    }
    m_state.store(State::Preparing);

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

    // 纯视频模式：不建音频编码器、封装器也不建音频流
    m_audioEnabled = m_audioParams.enabled && m_audioParams.sampleRate > 0 && m_audioParams.channels > 0;

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

    if (m_videoSource && m_videoSource->Open(m_videoParams.width, m_videoParams.height, m_videoParams.fps) < 0) {
        TG_LOGW("TGCapturer: 视频源打开失败（外部注入模式可忽略）");
    }
    if (m_audioEnabled && m_audioSource
        && m_audioSource->Open(m_audioParams.sampleRate, m_audioParams.channels) < 0) {
        TG_LOGW("TGCapturer: 音频源打开失败（将使用静音兜底）");
    }

    m_state.store(State::Prepared);
    if (m_onPrepared) {
        m_onPrepared(this);
    }
    return 0;
}

int TGCapturer::Start()
{
    if (m_state.load() != State::Prepared) {
        return -1;
    }
    m_state.store(State::Starting);

    m_muxer->Start();
    m_videoEncoder->Start();
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
    const State state = m_state.load();
    if (state == State::Idle || state == State::Stopping) {
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
    m_state.store(State::Idle);
    if (m_onStop) {
        m_onStop(0);
    }
    TG_LOGI("TGCapturer: 已停止");
}

void TGCapturer::ReleaseAll()
{
    m_ownedVideoSources.clear();
    m_ownedAudioSources.clear();
    m_videoSource = nullptr;
    m_audioSource = nullptr;
    m_audioSources.clear();
    {
        std::lock_guard<std::mutex> lock(m_audioMixMutex);
        m_audioLatest.clear();
    }
}

// ------------------------------------------------------------------ 帧流转
void TGCapturer::OnVideoFrame(core::FramePtr frame)
{
    if (!frame || m_state.load() != State::Running) {
        return;
    }
    if (m_videoMuted.load()) {
        return;
    }

    // 背压：编码器积压过多则丢帧，保证直播实时性
    constexpr size_t kMaxEncoderPending = 8;
    if (m_videoEncoder->Pending() >= kMaxEncoderPending) {
        return;
    }

    if (frame->ptsUs < 0) {
        frame->ptsUs = frame->captureUs;
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

void TGCapturer::OnAudioFrame(core::FramePtr frame)
{
    if (!frame || !m_audioEnabled || m_state.load() != State::Running) {
        return;
    }
    if (frame->ptsUs < 0) {
        frame->ptsUs = frame->captureUs;
    }
    m_bus.Dispatch("audio_encoder", std::move(frame));
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
        // iOS CVPixelBuffer / Windows D3D 纹理：零拷贝直送硬编
        frame->hwKind = core::HwKind::CVPixelBuffer;
        frame->hwHandle = pixelBuffer;
    } else if (data) {
        const size_t bytes = static_cast<size_t>(stride > 0 ? stride : width) * height * 3 / 2;
        auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
        memcpy(buffer.get(), data, bytes);
        frame->planes[0] = buffer;
        frame->strides[0] = stride > 0 ? stride : width;
    } else {
        return -2;
    }
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
    OnAudioFrame(std::move(frame));
    return 0;
}

// ------------------------------------------------------------------ 运行期控制
bool TGCapturer::MuteAudio(bool mute, AudioCapSourceType channel)
{
    if (m_audioSource) {
        auto* win = dynamic_cast<capture::AudioSourceBase*>(m_audioSource);
        if (win) {
            // 静音由采集侧置零实现
        }
    }
    return true;
}

bool TGCapturer::MuteVideo(bool mute)
{
    m_videoMuted.store(mute);
    return true;
}

int TGCapturer::SetPauseInfo(const PauseParams& params)
{
    TG_LOGW("TGCapturer: 暂停静帧能力待实现（pause=%s）", params.picture.c_str());
    return 0;
}

int TGCapturer::SetupWatermarkInfo(const WatermarkParams& params)
{
    TG_LOGW("TGCapturer: 水印能力待实现（index=%u）", params.index);
    return 0;
}

int TGCapturer::SetAVTimeout(int64_t audioTimeoutMs, int64_t videoTimeoutMs)
{
    m_audioTimeoutMs = audioTimeoutMs;
    m_videoTimeoutMs = videoTimeoutMs;
    m_videoEncoder->SetMaxLagUs(videoTimeoutMs * 1000);
    return 0;
}

// ------------------------------------------------------------------ 统计
std::string TGCapturer::GetStatsInfo(StatsInfoType type)
{
    char buffer[512];
    switch (type) {
        case StatsInfoType::bitrateInfo:
            snprintf(buffer, sizeof(buffer), "{\"bitrate_bps\":%.0f}", m_muxer ? m_muxer->TrafficBps() : 0.0f);
            break;
        case StatsInfoType::videoInfo:
            snprintf(buffer, sizeof(buffer),
                     "{\"codec\":\"%s\",\"encoded\":%llu,\"dropped\":%llu,\"pending\":%zu}",
                     m_videoEncoder ? m_videoEncoder->CodecName().c_str() : "",
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->EncodedFrames()) : 0ULL,
                     m_videoEncoder ? static_cast<unsigned long long>(m_videoEncoder->DroppedFrames()) : 0ULL,
                     m_videoEncoder ? m_videoEncoder->Pending() : 0);
            break;
        case StatsInfoType::audioInfo:
            snprintf(buffer, sizeof(buffer), "{\"encoded\":%llu,\"pending\":%zu}",
                     m_audioEncoder ? static_cast<unsigned long long>(m_audioEncoder->EncodedFrames()) : 0ULL,
                     m_audioEncoder ? m_audioEncoder->Pending() : 0);
            break;
        default:
            snprintf(buffer, sizeof(buffer), "{%s}", m_muxer ? m_muxer->Stats().c_str() : "");
            break;
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
