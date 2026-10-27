// 开播门面实现：串联 采集 -> 编码 -> 封装推流，并保持对外接口不变
#ifndef TGCAPTURE_TGCAPTURER_H
#define TGCAPTURE_TGCAPTURER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TGCapture/ITGCapturer.h"
#include "capture/Source.h"
#include "codec/AudioEncoder.h"
#include "codec/VideoEncoder.h"
#include "core/AVSyncClock.h"
#include "core/AudioMixer.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "core/PauseFrame.h"
#include "core/Watermark.h"
#include "mux/Muxer.h"

namespace TGCapture
{

class TGCapturer : public ITGCapturer
{
public:
    explicit TGCapturer(GameID gameId);
    ~TGCapturer() override;

    // ---- ITGCapturer ----
    bool IsSupported(CaptureFeature feature) const override;

    int EnableMicrophoneCapture(bool usingGVoice = false) override;
    int EnableSpeakerCapture() override;
    IVideoSource* CreateScreenCapture(int index = 0, VideoCapMode mode = DXGI,
                                      VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) override;
    IVideoSource* CreateWindowCapture(void* handle, VideoCapMode mode = WGC,
                                      VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) override;
    IVideoSource* CreateLocalFileCapture(int index, const std::string& filePath, bool loopPlay = false) override;

    int AddAudioSource(IAudioSource* source) override;
    int AddVideoSource(IVideoSource* source) override;

    int SetupAudioEncoder(const AudioEncoderParams& params) override;
    int SetupVideoEncoder(const VideoEncoderParams& params) override;
    int SetupMuxer(const MuxerParams& params) override;

    void SetOnPreparedCallback(OnPreparedCallback cb) override;
    void SetOnErrorCallback(OnErrorCallback cb) override;
    void SetOnStartCallback(OnStartCallback cb) override;
    void SetOnStopCallback(OnStopCallback cb) override;
    void SetOnNotifyCallback(OnNotifyCallback cb) override;

    void SetPrivateData(const std::string& key, void* data) override;

    int Prepare() override;
    int Start() override;
    void Stop() override;

    bool MuteAudio(bool mute, AudioCapSourceType channel = TypeOfANone) override;
    bool MuteVideo(bool mute) override;
    int SetPauseInfo(const PauseParams& params) override;
    int SetupWatermarkInfo(const WatermarkParams& params) override;
    int SetAVTimeout(int64_t audioTimeoutMs, int64_t videoTimeoutMs) override;

    std::string GetStatsInfo(StatsInfoType type) override;
    float GetTrafficStats() override;

    int PushExternalVideoFrame(const uint8_t* data, int width, int height, int stride,
                               int64_t ptsUs, void* pixelBuffer = nullptr) override;
    int PushExternalAudioFrame(AudioCapSourceType type, const uint8_t* data, int size,
                               int sampleRate, int channels, int64_t ptsUs) override;

private:
    enum class State
    {
        Idle,
        Preparing,
        Prepared,
        Starting,
        Running,
        Stopping,
    };

    void OnVideoFrame(core::FramePtr frame);
    // 外部注入的音频（PushExternalAudioFrame）也进混流器
    void OnAudioFrame(AudioCapSourceType type, core::FramePtr frame);
    void FireError(int32_t what, int32_t extra, const std::string& msg);
    void FireNotify(NotifyMsgType type, const std::string& content, int64_t data);
    void ReleaseAll();

    // 多路音频（麦克风 + 扬声器 …）：统一进 AudioMixer，由混音线程产出一路
    void OnAudioFrameFrom(capture::AudioSourceBase* source, core::FramePtr frame);
    void StartAudioMixer();
    void StopAudioMixer();
    void MixLoop(std::stop_token token);
    int ChannelIdOf(capture::AudioSourceBase* source) const;

private:
    GameID m_gameId = GameID::GAME_ID_DEFAULT;
    std::atomic<State> m_state{State::Idle};
    // 生命周期互斥：Prepare / Start / Stop 三个入口串行化，避免快速启停时
    // "Stop 在 Starting 态跑完、Start 随后才启动线程"导致的野线程。
    //
    // 必须用 recursive_mutex：Prepare() 内部要触发 m_onPrepared 回调，
    // 而宿主的标准用法正是在回调里调 Start()（demo 就是这么写的），
    // 普通 mutex 会在同线程重入时自死锁。
    std::recursive_mutex m_lifecycleMutex;

    // 参数
    VideoEncoderParams m_videoParams;
    AudioEncoderParams m_audioParams;
    MuxerParams m_muxParams;
    bool m_videoParamsSet = false;
    bool m_audioParamsSet = false;
    bool m_muxParamsSet = false;
    bool m_audioEnabled = false;   // Prepare 时按 m_audioParams.enabled 解析，决定是否有音频流

    // 源（不拥有：由调用方 Add* 传入；创建型接口由本对象持有）
    std::vector<std::shared_ptr<capture::VideoSourceBase>> m_ownedVideoSources;
    std::vector<std::shared_ptr<capture::AudioSourceBase>> m_ownedAudioSources;
    capture::VideoSourceBase* m_videoSource = nullptr;  // 非拥有
    capture::AudioSourceBase* m_audioSource = nullptr;  // 非拥有（首个/主要音频源）
    std::vector<capture::AudioSourceBase*> m_audioSources;   // 全部音频源（麦克风 + 扬声器）

    // 多源混流（core/AudioMixer：按采样点加和 + 每路 PI/BURST 时钟校正）
    std::unique_ptr<core::AudioMixer> m_mixer;
    int m_audioFrameSize = 1024;
    std::jthread m_mixThread;

    // 平台采集句柄（Android：MediaProjection jobject 全局引用，由 SetPrivateData 传入）
    void* m_mediaProjection = nullptr;

    // 性能模式（Cpu / Gpu）：影响采集是否保留硬件帧、编码器是否走硬件直连
    PerformanceMode m_perfMode = PerformanceMode::Cpu;

    // 音画同步（独立模块，见 core/AVSyncClock.h）
    std::shared_ptr<core::AudioClock> m_audioClock;
    std::shared_ptr<core::VideoPtsGenerator> m_videoPtsGen;
    std::shared_ptr<core::VideoOnlyTimeline> m_videoOnlyTimeline;
    int64_t m_videoIntervalUs = 33333;
    uint64_t m_syncDropped = 0;
    // 编码前背压丢帧（编码器积压 >= kMaxEncoderPending）：之前不计数，E 场景丢 117 帧统计里不可见
    std::atomic<uint64_t> m_bpDropped{0};
    // 音频主时钟首帧等待：音频未就绪时视频不发帧（避免就绪后 PTS 回退被整段丢弃）
    // 对齐 MediaX：fallback 判定每帧独立进行，不保留"已降级"状态；
    // 只保留一个 **节流**日志闸门避免刷屏，不影响后续"音频恢复 → 自动切回主时钟"。
    int64_t m_audioWaitStartUs = 0;

    // 内核
    std::shared_ptr<codec::VideoEncoder> m_videoEncoder;
    std::shared_ptr<codec::AudioEncoder> m_audioEncoder;
    std::shared_ptr<Muxer> m_muxer;
    std::shared_ptr<core::AVSync> m_sync;
    core::Bus m_bus;

    // 回调
    OnPreparedCallback m_onPrepared;
    OnErrorCallback m_onError;
    OnStartCallback m_onStart;
    OnStopCallback m_onStop;
    OnNotifyCallback m_onNotify;

    // 运行期
    std::atomic_bool m_videoMuted{false};
    std::atomic_bool m_firstFrameSent{false};

    // 暂停静帧（对齐 MediaX ESGeneratePicture::SetPausePic）
    std::shared_ptr<core::PauseFrame> m_pauseFrame;
    PauseParams m_pauseParams;

    // 水印（对齐 MediaX ESVPicFilter）
    std::shared_ptr<core::Watermark> m_watermark;
    // 外部注入视频是否沿用调用方自带的 PTS（VideoEncoderParams::videoUseExternalPts）。
    // Prepare 写、采集/注入线程读，用 atomic 避免跨线程读写同一 bool。
    std::atomic_bool m_useExternalVideoPts{false};
    // 外部注入音频按 AudioCapSourceType 分路，每种类型独占一个混流通道。
    // 不能复用 0 号：那是内部麦克风源，两路共用一个通道时钟会互相拉扯时间戳；
    // 外部多路（如游戏音频 + 游戏麦克风）也必须各占一路，否则会被串成一条时间轴，
    // 结果不是"混音"而是"交替输出"。
    std::unordered_map<int, int> m_externalAudioChannels;
    std::mutex m_externalAudioMutex;
    int64_t m_audioTimeoutMs = 3000;
    int64_t m_videoTimeoutMs = 3000;
    int64_t m_lastVideoUs = 0;
};

}  // namespace TGCapture

#endif  // TGCAPTURE_TGCAPTURER_H
