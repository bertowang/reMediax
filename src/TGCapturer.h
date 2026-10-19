// 开播门面实现：串联 采集 -> 编码 -> 封装推流，并保持对外接口不变
#ifndef TGCAPTURE_TGCAPTURER_H
#define TGCAPTURE_TGCAPTURER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TGCapture/ITGCapturer.h"
#include "capture/Source.h"
#include "codec/AudioEncoder.h"
#include "codec/VideoEncoder.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "mux/Muxer.h"

namespace TGCapture
{

class TGCapturer : public ITGCapturer
{
public:
    explicit TGCapturer(GameID gameId);
    ~TGCapturer() override;

    // ---- ITGCapturer ----
#ifdef _WIN32
    int EnableMicrophoneCapture(bool usingGVoice = false) override;
    int EnableSpeakerCapture() override;
    IVideoSource* CreateScreenCapture(int index = 0, VideoCapMode mode = DXGI,
                                      VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) override;
    IVideoSource* CreateWindowCapture(void* hwnd, VideoCapMode mode = WGC,
                                      VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) override;
#endif
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
    void OnAudioFrame(core::FramePtr frame);
    void FireError(int32_t what, int32_t extra, const std::string& msg);
    void FireNotify(NotifyMsgType type, const std::string& content, int64_t data);
    void ReleaseAll();

private:
    GameID m_gameId = GameID::GAME_ID_DEFAULT;
    std::atomic<State> m_state{State::Idle};
    std::mutex m_mutex;

    // 参数
    VideoEncoderParams m_videoParams;
    AudioEncoderParams m_audioParams;
    MuxerParams m_muxParams;
    bool m_videoParamsSet = false;
    bool m_audioParamsSet = false;
    bool m_muxParamsSet = false;

    // 源（不拥有：由调用方 Add* 传入；创建型接口由本对象持有）
    std::vector<std::shared_ptr<capture::VideoSourceBase>> m_ownedVideoSources;
    std::vector<std::shared_ptr<capture::AudioSourceBase>> m_ownedAudioSources;
    capture::VideoSourceBase* m_videoSource = nullptr;  // 非拥有
    capture::AudioSourceBase* m_audioSource = nullptr;  // 非拥有

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
    int64_t m_audioTimeoutMs = 3000;
    int64_t m_videoTimeoutMs = 3000;
    int64_t m_lastVideoUs = 0;
};

}  // namespace TGCapture

#endif  // TGCAPTURE_TGCAPTURER_H
