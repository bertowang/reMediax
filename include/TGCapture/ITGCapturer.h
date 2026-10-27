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

// 错误处理约定（细节见 TGCapturerInterface.h 的 TGCaptureError）
//   - 所有返回 int 的接口：0 = 成功，负数 = TGCaptureError 中的某一项，不存在"未定义的负值"。
//     返回指针的接口（Create*Capture）用 nullptr 表示失败，失败原因写日志。
//   - 配置期 / 生命周期的失败：既是返回值，也会经 OnErrorCallback 上报一次（便于统一在一处处理）。
//   - 运行期（Start 之后）的失败：没有返回值可用，只经 OnErrorCallback 上报，码段为 -100 以下。
//   - OnErrorCallback 的 extra 一律是底层原始错误码（AVERROR / 平台码），只用于排障。
class TGCAPTURE_API ITGCapturer
{
public:
    static ITGCapturer* Create(GameID gameId = GameID::GAME_ID_DEFAULT);
    static void Destroy(ITGCapturer* capturer);

    virtual ~ITGCapturer() = default;

    // ---------------- 采集源（Start 之前配置） ----------------
    // 三端均为同一组接口；不支持的能力由 IsSupported 提前告知，
    // 直接调用则返回 TGErr_NotSupported（创建型接口返回 nullptr）。
    // Windows : Screen / Window / Microphone / Speaker 全支持
    // Android : Screen（MediaProjection，需先 SetPrivateData 传 token）/ Microphone / Speaker(API 29+)
    // iOS     : Screen（ReplayKit，应用内）/ Microphone；无窗口、无系统内录
    virtual bool IsSupported(CaptureFeature feature) const = 0;

    // 麦克风：一律走系统采集（WASAPI / AudioRecord / AVAudioSession），
    // usingGVoice 仅为兼容老调用方保留，传 true 也按系统麦克风处理。
    virtual int EnableMicrophoneCapture(bool usingGVoice = false) = 0;
    virtual int EnableSpeakerCapture() = 0;   // Windows: WASAPI loopback；Android: AudioPlaybackCapture
    virtual IVideoSource* CreateScreenCapture(int index = 0, VideoCapMode mode = DXGI,
                                              VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) = 0;
    virtual IVideoSource* CreateWindowCapture(void* handle, VideoCapMode mode = WGC,
                                              VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0) = 0;
    virtual IVideoSource* CreateLocalFileCapture(int index, const std::string& filePath, bool loopPlay = false) = 0;

    // 失败：TGErr_InvalidArgument（空指针，或对象不是本 SDK 创建的源）
    virtual int AddAudioSource(IAudioSource* source) = 0;
    virtual int AddVideoSource(IVideoSource* source) = 0;

    // 三个 Setup 必须都成功调用过，否则 Prepare 返回 TGErr_NotConfigured。
    // 参数明显非法（尺寸 / 帧率 / 码率 / 采样率 ≤ 0，推流地址为空）时返回 TGErr_InvalidArgument，
    // 且该组参数**不会被记录**——与"没调用过"等价，不会带着坏参数走到 Prepare 再失败。
    virtual int SetupAudioEncoder(const AudioEncoderParams& params) = 0;
    virtual int SetupVideoEncoder(const VideoEncoderParams& params) = 0;
    virtual int SetupMuxer(const MuxerParams& params) = 0;

    // ---------------- 回调 ----------------
    using OnPreparedCallback = std::function<void(ITGCapturer*)>;
    // what = TGCaptureError，extra = 底层原始错误码（AVERROR / 平台码，仅排障用）
    using OnErrorCallback = std::function<void(int32_t what, int32_t extra, const char* errMsg)>;
    using OnStartCallback = std::function<void()>;
    // err = 本轮运行期最后一次上报的错误码（0 = 全程无错误）。
    // Stop 会先把积压的 OnError / OnNotify 投递完，再回调本函数 —— 宿主看到的顺序一定是"…错误…OnStop"
    using OnStopCallback = std::function<void(int32_t err)>;
    using OnNotifyCallback = std::function<void(NotifyMsgType type, const std::string& content, int64_t data)>;

    virtual void SetOnPreparedCallback(OnPreparedCallback cb) = 0;
    virtual void SetOnErrorCallback(OnErrorCallback cb) = 0;
    virtual void SetOnStartCallback(OnStartCallback cb) = 0;
    virtual void SetOnStopCallback(OnStopCallback cb) = 0;
    virtual void SetOnNotifyCallback(OnNotifyCallback cb) = 0;

    virtual void SetPrivateData(const std::string& key, void* data) = 0;

    // ---------------- 生命周期 ----------------
    // Prepare 失败：TGErr_InvalidState / NotConfigured / OpenVideoEncoder / OpenAudioEncoder
    //               / OpenMuxer / OpenVideoSource
    // Start   失败：TGErr_InvalidState（未 Prepare）/ TGErr_StartFailed
    virtual int Prepare() = 0;
    virtual int Start() = 0;
    virtual void Stop() = 0;

    // ---------------- 运行期控制 ----------------
    virtual bool MuteAudio(bool mute, AudioCapSourceType channel = TypeOfANone) = 0;
    virtual bool MuteVideo(bool mute) = 0;
    virtual int SetPauseInfo(const PauseParams& params) = 0;
    virtual int SetupWatermarkInfo(const WatermarkParams& params) = 0;
    // 断流判定阈值：某一路超过对应时长没有新数据，上报 TGErr_AudioCaptureStalled /
    // TGErr_VideoCaptureStalled，恢复时发对应的 Notify。≤0 = 关闭该路看护。
    // 非法入参返回 TGErr_InvalidArgument。可在运行期调用，立即生效。
    virtual int SetAVTimeout(int64_t audioTimeoutMs, int64_t videoTimeoutMs) = 0;

    virtual std::string GetStatsInfo(StatsInfoType type) = 0;
    virtual float GetTrafficStats() = 0;      // bps

    // ---------------- 外部帧注入（Android/iOS/游戏引擎） ----------------
    // 两个接口的失败码：
    //   TGErr_InvalidState    未 Start（或已 Stop）
    //   TGErr_InvalidArgument data 为空 / size ≤ 0 / 宽高 ≤ 0 / stride < width / 采样率或声道数 ≤ 0
    //   TGErr_NotSupported    传了 pixelBuffer，但当前平台没有硬件帧通路（仅 Windows / iOS 支持）
    // 视频：pixelBuffer 在 iOS 为 CVPixelBufferRef，Android 为 nullptr（走 Surface）
    virtual int PushExternalVideoFrame(const uint8_t* data, int width, int height, int stride,
                                       int64_t ptsUs, void* pixelBuffer = nullptr) = 0;
    // 音频：data 为交错 S16 PCM，size 为字节数。
    //
    // ptsUs 约定（内核据此测量各路设备时钟相对墙钟的漂移并在样本域补偿）：
    //   - 单位微秒，取本块第一个采样的**采集时刻**；
    //   - 与内核同一时钟域：单调时钟（Linux/Android CLOCK_MONOTONIC，iOS mach_absolute_time 换算，
    //     Windows QueryPerformanceCounter 换算），起点任意，只看差值；
    //   - 严格递增；推进速率须与墙钟一致（不要传"按采样数累加"的 PTS —— 那样漂移恒为 0，补偿永不生效）；
    //   - 不知道采集时刻时传 -1，内核用到达时刻代替。
    // 合理性校验：时间戳不递增，或 5s 窗口内推进速率偏离墙钟 > 5% 时，内核告警一次并对该路改用到达时刻。
    // 输出音频 PTS 由内核按输出样本数生成，不直接使用 ptsUs。
    virtual int PushExternalAudioFrame(AudioCapSourceType type, const uint8_t* data, int size,
                                       int sampleRate, int channels, int64_t ptsUs) = 0;
};

}  // namespace TGCapture

#endif  // TGCAPTURE_ITGCAPTURER_H
