// 开播门面实现：串联 采集 -> 编码 -> 封装推流，并保持对外接口不变
#ifndef TGCAPTURE_TGCAPTURER_H
#define TGCAPTURE_TGCAPTURER_H

#include <atomic>
#include <condition_variable>
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
#include "core/Health.h"
#include "core/PauseFrame.h"
#include "core/Watermark.h"
#include "mux/Muxer.h"
#include "nodes/AudioNodes.h"
#include "nodes/VideoNodes.h"

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

    void SetOnPreviewFrameCallback(OnPreviewFrameCallback cb) override;
    int StartPreview() override;
    void StopPreview() override;
    bool IsPreviewing() const override;

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
        Previewing,   // 只有预览（视频采集 + 合成在跑），没有推流
    };

    // Prepare / Start 的实际工作（持生命周期锁）。失败时通过 err 带出要上报的错误，
    // 由外层在**解锁之后**同步触发 —— 宿主回调永远不在内部锁里被调用。
    // what 既是上报码也是接口返回值（见 TGCaptureError），extra 是底层原始错误码
    struct PrepareError
    {
        int32_t what = 0;
        int32_t extra = 0;
        std::string message;
    };
    int PrepareLocked(PrepareError& err);
    int StartLocked(PrepareError& err);
    void StopLocked();
    // 按数据流逆序拆除整条链路（停线程 → 关资源）。Stop 与 Prepare 失败回滚共用同一套顺序，
    // 避免回滚路径另写一份、漏掉某个节点。对未启动/未打开的节点是无副作用的空操作。
    // keepPreview：预览开着时保留 preview 组（视频采集 + 合成）继续运行
    void TeardownLocked(bool keepPreview);
    // 打开主画面源（含 Windows GPU 链路的采集侧协商）。已打开（预览期间）直接返回 0
    int OpenVideoSourceLocked(PrepareError& err);
    // 停止并关闭 preview 组（视频采集 + 合成），并解除预览连线
    void ClosePreviewGroupLocked();
    // 预览 / 推流都结束后的状态：预览开着回 Previewing，否则 Idle
    State IdleOrPreviewing() const { return m_previewOn ? State::Previewing : State::Idle; }
    // 等待在途的 Stop（含锁外收尾）结束。返回 false 表示调用方自己就是停机线程，不应再停。
    bool WaitStopSettled(std::unique_lock<std::mutex>& lock);

    // 绑定主画面源：门面记录 + vsrc.main 节点接管其产帧回调。视频源已打开（预览 / 推流中）时拒绝换源
    int BindVideoSource(capture::VideoSourceBase* source);
    int BindVideoSourceLocked(capture::VideoSourceBase* source);
    // 外部注入的音频（PushExternalAudioFrame）也进混流器
    void OnAudioFrame(AudioCapSourceType type, core::FramePtr frame);
    // 经总线发布（异步：由事件分发线程投递，永不阻塞调用线程）
    void FireError(int32_t what, int32_t extra, const std::string& msg);
    void FireNotify(NotifyMsgType type, const std::string& content, int64_t data);
    // 总线事件 → 内核自身的联动反应 + 宿主回调
    void OnBusEvent(const core::BusEvent& event);
    void ReleaseAll();

    // 多路音频（麦克风 + 扬声器 + 外部注入 …）：每路一个 asrc.* 节点接到 audio_mixer，
    // 由混音节点的时钟线程产出一路（见 nodes/AudioNodes.h）
    std::string UniqueAudioNodeName(const std::string& base) const;
    // 拆掉外部注入音频的 asrc.ext.* 节点（Prepare 前 / 停止后），下一轮按首帧重新建
    void RemoveExternalAudioNodes();
    // 混音线程每 10s 回调：打印同步 / 混流 / 总线累计状态
    void LogHeartbeat();

    // 断流看护：采集类故障（设备丢失 / 权限被撤销 / 采集重建失败 / 上层停推）没有失败返回值，
    // 共同表现就是"不再有数据"。在内核侧统一探测，比指望每个平台各自上报可靠得多
    void StartWatchdog();
    void StopWatchdog();
    void WatchLoop(std::stop_token token);

private:
    GameID m_gameId = GameID::GAME_ID_DEFAULT;
    std::atomic<State> m_state{State::Idle};
    // 生命周期互斥：Prepare / Start / Stop 三个入口串行化，避免快速启停时
    // "Stop 在 Starting 态跑完、Start 随后才启动线程"导致的野线程。
    //
    // 用普通 mutex 而非 recursive：所有宿主回调（Prepared / Started / Stopped / Error）
    // 都已经移到解锁之后触发，本类内部不存在同线程重入路径。
    // 这里刻意不留 recursive 的余地 —— 它会把"在持锁状态下回调宿主"这种缺陷
    // 掩盖成"能跑但状态被嵌套修改"，而不是在测试里立刻暴露。
    std::mutex m_lifecycleMutex;
    // Stop 的收尾（Drain + 发 Stopped）必须在锁外做（回调不能在锁内触发），
    // 于是出现一个空窗：此时锁已放开，别的线程调 Stop 会看到 state==Idle 直接返回，
    // 以为"已经停干净了"转头就 delete，而收尾阶段仍在回调宿主 → 析构与回调并发。
    // 用下面三个成员把收尾也纳入"停机未完成"：其它线程等它结束，
    // 发起停机的线程自己（宿主在 OnStop 里再调 Stop）则直接放行，避免自等。
    bool m_stopping = false;
    std::thread::id m_stopOwner;
    std::condition_variable m_stopSettled;

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

    // 平台采集句柄（Android：MediaProjection jobject 全局引用，由 SetPrivateData 传入）
    void* m_mediaProjection = nullptr;

    // 性能模式（Cpu / Gpu）：影响采集是否保留硬件帧、编码器是否走硬件直连
    PerformanceMode m_perfMode = PerformanceMode::Cpu;

    // 音画同步（独立模块，见 core/AVSyncClock.h）
    std::shared_ptr<core::AudioClock> m_audioClock;
    std::shared_ptr<core::VideoPtsGenerator> m_videoPtsGen;
    std::shared_ptr<core::VideoOnlyTimeline> m_videoOnlyTimeline;

    // 总线：必须声明在所有节点之前 —— 成员按声明逆序析构，总线最后销毁，
    // 节点线程退出过程中发布的事件才不会落在已销毁的对象上
    core::Bus m_bus;

    // 内核（总线节点）。数据流：
    //   asrc.* → audio_mixer → audio_encoder ──────────────────────┐
    //   vsrc.main → video_compositor → video_sync → video_encoder ─┴→ muxer
    std::shared_ptr<codec::VideoEncoder> m_videoEncoder;
    std::shared_ptr<codec::AudioEncoder> m_audioEncoder;
    std::shared_ptr<Muxer> m_muxer;
    std::shared_ptr<core::AVSync> m_sync;
    // 音频链路：混音节点（按采样点加和 + 每路 PI/BURST 时钟校正，自带混音时钟线程）与各路源节点
    std::shared_ptr<nodes::AudioMixerNode> m_audioMixer;
    std::vector<std::shared_ptr<nodes::AudioSourceNode>> m_audioSourceNodes;   // 原生源（AddAudioSource）
    // 视频链路：主画面源 → 图层合成（暂停图 / 水印）→ 编码前同步（背压 + 音画同步）
    std::shared_ptr<nodes::VideoSourceNode> m_vsrcMain;
    std::shared_ptr<nodes::VideoCompositorNode> m_compositor;
    std::shared_ptr<nodes::VideoSyncNode> m_videoSync;
    std::shared_ptr<nodes::PreviewSinkNode> m_previewSink;
    // 预览开关（持 m_lifecycleMutex 读写；IsPreviewing 无锁读用 atomic）与主画面源是否已打开
    std::atomic_bool m_previewOn{false};
    bool m_videoSourceOpened = false;

    // 宿主回调：设置与调用可能在不同线程，用互斥保护。调用时先拷贝出来再在锁外执行
    std::mutex m_callbackMutex;
    OnPreparedCallback m_onPrepared;
    OnErrorCallback m_onError;
    OnStartCallback m_onStart;
    OnStopCallback m_onStop;
    OnNotifyCallback m_onNotify;

    // 暂停静帧（对齐 MediaX ESGeneratePicture::SetPausePic）
    std::shared_ptr<core::PauseFrame> m_pauseFrame;
    PauseParams m_pauseParams;

    // 水印（对齐 MediaX ESVPicFilter）
    std::shared_ptr<core::Watermark> m_watermark;
    // 外部注入视频是否沿用调用方自带的 PTS（VideoEncoderParams::videoUseExternalPts）。
    // Prepare 写、采集/注入线程读，用 atomic 避免跨线程读写同一 bool。
    std::atomic_bool m_useExternalVideoPts{false};
    // 外部注入音频按 AudioCapSourceType 分路，每种类型独占一个 asrc.ext.<type> 节点（= 一个混流通道）。
    // 不能与内部麦克风源共用：两路共用一个通道时钟会互相拉扯时间戳；
    // 外部多路（如游戏音频 + 游戏麦克风）也必须各占一路，否则会被串成一条时间轴，
    // 结果不是"混音"而是"交替输出"。
    std::unordered_map<int, std::shared_ptr<nodes::AudioSourceNode>> m_externalAudioNodes;
    std::mutex m_externalAudioMutex;
    // 断流阈值（SetAVTimeout 可改）与看护状态
    std::atomic<int64_t> m_audioTimeoutMs{3000};
    std::atomic<int64_t> m_videoTimeoutMs{3000};
    core::StallWatch m_videoWatch;
    core::StallWatch m_audioWatch;
    // 看护线程声明在最后：它访问 m_bus / 编码器节点 / 同步时钟，
    // 逆序析构保证先 join 线程、再销毁线程正在用的东西（混音线程归 audio_mixer 节点，Stop 时已 join）
    std::jthread m_watchThread;
    // 本轮运行期最后一次上报的错误码，Stop 时带给 OnStopCallback（0 = 全程无错误）
    std::atomic<int32_t> m_lastRuntimeError{0};
};

}  // namespace TGCapture

#endif  // TGCAPTURE_TGCAPTURER_H
