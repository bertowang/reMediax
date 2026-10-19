// TGCapture 开播 SDK —— 门面接口（与既有 IESMediaXCapturer 语义一致）
#ifndef TGCAPTURE_ITGCAPTURER_H
#define TGCAPTURE_ITGCAPTURER_H

#include <functional>
#include <string>

#include "TGCapture/TGCapturerInterface.h"
#include "TGCapture/Export.h"

namespace TGCapture
{

class TGCAPTURE_API ITGCapturerMgr
{
public:
    static int Init(int logLevel = 2, const std::string& logDir = "", bool printLog = false);
    static void UnInit();
    static const char* GetVersion();
};

class TGCAPTURE_API ITGCapturer
{
public:
    static ITGCapturer* Create(GameID gameId = GameID::GAME_ID_DEFAULT);
    static void Destroy(ITGCapturer* capturer);

    virtual ~ITGCapturer() = default;

    // ---------------- 采集源（Start 之前配置） ----------------
#ifdef _WIN32
    virtual int EnableMicrophoneCapture(bool usingGVoice = false) = 0;
    virtual int EnableSpeakerCapture() = 0;
    virtual IVideoSource* CreateScreenCapture(int index = 0, VideoCapMode mode = DXGI,
                                              VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) = 0;
    virtual IVideoSource* CreateWindowCapture(void* hwnd, VideoCapMode mode = WGC,
                                              VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) = 0;
#endif
    virtual IVideoSource* CreateLocalFileCapture(int index, const std::string& filePath, bool loopPlay = false) = 0;

    virtual int AddAudioSource(IAudioSource* source) = 0;
    virtual int AddVideoSource(IVideoSource* source) = 0;

    virtual int SetupAudioEncoder(const AudioEncoderParams& params) = 0;
    virtual int SetupVideoEncoder(const VideoEncoderParams& params) = 0;
    virtual int SetupMuxer(const MuxerParams& params) = 0;

    // ---------------- 回调 ----------------
    using OnPreparedCallback = std::function<void(ITGCapturer*)>;
    using OnErrorCallback = std::function<void(int32_t what, int32_t extra, const char* errMsg)>;
    using OnStartCallback = std::function<void()>;
    using OnStopCallback = std::function<void(int32_t err)>;
    using OnNotifyCallback = std::function<void(NotifyMsgType type, const std::string& content, int64_t data)>;

    virtual void SetOnPreparedCallback(OnPreparedCallback cb) = 0;
    virtual void SetOnErrorCallback(OnErrorCallback cb) = 0;
    virtual void SetOnStartCallback(OnStartCallback cb) = 0;
    virtual void SetOnStopCallback(OnStopCallback cb) = 0;
    virtual void SetOnNotifyCallback(OnNotifyCallback cb) = 0;

    virtual void SetPrivateData(const std::string& key, void* data) = 0;

    // ---------------- 生命周期 ----------------
    virtual int Prepare() = 0;
    virtual int Start() = 0;
    virtual void Stop() = 0;

    // ---------------- 运行期控制 ----------------
    virtual bool MuteAudio(bool mute, AudioCapSourceType channel = TypeOfANone) = 0;
    virtual bool MuteVideo(bool mute) = 0;
    virtual int SetPauseInfo(const PauseParams& params) = 0;
    virtual int SetupWatermarkInfo(const WatermarkParams& params) = 0;
    virtual int SetAVTimeout(int64_t audioTimeoutMs, int64_t videoTimeoutMs) = 0;

    virtual std::string GetStatsInfo(StatsInfoType type) = 0;
    virtual float GetTrafficStats() = 0;      // bps

    // ---------------- 外部帧注入（Android/iOS/游戏引擎） ----------------
    // 视频：pixelBuffer 在 iOS 为 CVPixelBufferRef，Android 为 nullptr（走 Surface）
    virtual int PushExternalVideoFrame(const uint8_t* data, int width, int height, int stride,
                                       int64_t ptsUs, void* pixelBuffer = nullptr) = 0;
    virtual int PushExternalAudioFrame(AudioCapSourceType type, const uint8_t* data, int size,
                                       int sampleRate, int channels, int64_t ptsUs) = 0;
};

}  // namespace TGCapture

#endif  // TGCAPTURE_ITGCAPTURER_H
