// TGCapture 开播 SDK —— 门面接口（与既有 IESMediaXCapturer 语义一致）
#ifndef TGCAPTURE_ITGCAPTURER_H
#define TGCAPTURE_ITGCAPTURER_H

#include <functional>
#include <string>
#include <vector>

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

    // ---------------- 编码器探测 ----------------
    // 编进 ffmpeg 不等于本机能用：硬编还取决于设备 / 系统 / 驱动。这里用最小参数真正打开一次判定，
    // 结果进程内缓存。硬编首次探测会真的创建一次编码会话（几十 ~ 几百 ms），别在 UI 线程上批量调。
    // 不探测也能开播：Setup 时请求的编码器打不开会自动回落（视频 → libopenh264，音频 → aac），
    // 并在 Prepare 期间发 EsCap_Notify_Encoder_Fallback。
    static bool IsEncoderAvailable(const std::string& name);
    // 本机可用的编码器，逗号分隔、按推荐顺序（视频：系统硬编在前）。
    // 只列 TGCapture 认识的 H.264 / AAC 编码器，例如 "h264_mediacodec,libopenh264" / "libfdk_aac,aac"。
    static std::string GetAvailableEncoders(bool video);

    // ---------------- 分辨率档位 ----------------
    // 内置档位（横屏宽x高 / 帧率 / 建议码率）：
    //   360p    640x360   30fps  0.8 Mbps        720p     1280x720   30fps  3.0 Mbps
    //   480p    854x480   30fps  1.2 Mbps        720p60   1280x720   60fps  4.5 Mbps
    //   540p    960x540   30fps  1.8 Mbps        1080p    1920x1080  30fps  5.0 Mbps
    //                                            1080p60  1920x1080  60fps  8.0 Mbps
    // 名字不区分大小写。portrait = true 时宽高互换（竖屏）。
    // 成功时改写 params 的 width / height / fps / vBitrate / ResolutionName，其余字段不动；
    // 名字不认识返回 false，params 不变。
    static bool ApplyResolutionPreset(const std::string& name, VideoEncoderParams& params, bool portrait = false);
    // 全部档位名，逗号分隔、按分辨率从低到高
    static std::string GetResolutionPresets();
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
    // 本地视频文件作主画面（mp4 / mov / flv 里的 H.264），按文件时间戳以实时节奏出帧，按原生尺寸采集、由编码器缩放。
    // loopPlay = true 播完从头再来；false 播完停在最后一帧（继续按帧率出帧，不触发断流看护），
    // 并发 EsCap_Notify_LocalFile_EOF（content = "video:main"）。index 仅用于日志。
    // 文件里的音轨不会自动加入：需要声音请另外 AddAudioSource({File, path})（两者各自按墙钟播放，
    // 同时开始时音画对齐，循环多遍后可能有一帧以内的累计偏差）。
    // 失败返回 nullptr：文件打不开 / 没有视频轨 / 解码器未编进；视频源已打开（预览 / 推流中）时不能更换主画面
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
    // 状态：Idle ─StartPreview→ Previewing ─Prepare→ Prepared ─Start→ Running
    //       Idle ─Prepare→ Prepared ─Start→ Running
    //       Running ─Stop→ Previewing（预览开着）/ Idle（预览没开）
    //       Previewing ─StopPreview→ Idle；Prepared / Running 时 StopPreview 只关预览，不影响推流
    // Prepare 失败：TGErr_InvalidState / NotConfigured / OpenVideoEncoder / OpenAudioEncoder
    //               / OpenMuxer / OpenVideoSource（预览开着时失败回到 Previewing，预览不中断）
    // Start   失败：TGErr_InvalidState（未 Prepare）/ TGErr_StartFailed
    virtual int Prepare() = 0;
    virtual int Start() = 0;
    // 停止推流。预览开着时回到预览（视频采集不中断，Android 不需要重新申请录屏授权）
    virtual void Stop() = 0;

    // ---------------- 预览 ----------------
    // 预览帧回调：在内核的预览投递线程上调用，只送最新一帧（宿主处理慢了就跳帧，不影响推流）。
    // 回调里不得调用 StopPreview / Destroy（会等待回调自身结束）
    using OnPreviewFrameCallback = std::function<void(const PreviewFrame& frame)>;
    virtual void SetOnPreviewFrameCallback(OnPreviewFrameCallback cb) = 0;
    // 开播前即可预览：打开视频采集（需先 SetupVideoEncoder 以确定采集尺寸 / 帧率，并已添加视频源）。
    // 可在 Idle / Prepared / Running 调用。失败：TGErr_NotConfigured / TGErr_OpenVideoSource / TGErr_InvalidState
    virtual int StartPreview() = 0;
    virtual void StopPreview() = 0;
    virtual bool IsPreviewing() const = 0;

    // ---------------- 运行期控制 ----------------
    virtual bool MuteAudio(bool mute, AudioCapSourceType channel = TypeOfANone) = 0;
    virtual bool MuteVideo(bool mute) = 0;
    virtual int SetPauseInfo(const PauseParams& params) = 0;
    virtual int SetupWatermarkInfo(const WatermarkParams& params) = 0;
    // 断流判定阈值：某一路超过对应时长没有新数据，上报 TGErr_AudioCaptureStalled /
    // TGErr_VideoCaptureStalled，恢复时发对应的 Notify。≤0 = 关闭该路看护。
    // 非法入参返回 TGErr_InvalidArgument。可在运行期调用，立即生效。
    virtual int SetAVTimeout(int64_t audioTimeoutMs, int64_t videoTimeoutMs) = 0;

    // ---------------- 运行期切换编码分辨率（推流不断） ----------------
    // 任意状态可调（需先 SetupVideoEncoder）：
    //   未开播（Idle / Previewing）：改写视频编码参数，下次 Prepare 生效；预览中采集尺寸一并跟随
    //   Prepared / Running        ：编码线程排空旧编码器 → 按新尺寸重开 → 首帧 IDR，
    //                                新的 SPS/PPS 带内交给封装（flv 重发 AVC sequence header，ts 在 IDR 前插入）。
    //                                切换期间不丢音频、视频时间轴连续；生效后发 EsCap_Notify_VideoMetaInfo_Changed
    // 采集侧：Android 屏幕采集按新尺寸改 VirtualDisplay（不重新申请授权）；其它平台按原生尺寸采集，
    //         由编码器缩放。预览画面随采集尺寸变化（PreviewFrame 的宽高以每帧为准）。
    // 暂停图 / 水印按每帧的实际尺寸自动重新缩放、定位。
    // 帧率不随切换改变（档位里的帧率只在填 VideoEncoderParams 时使用）。
    // 宽高必须 > 0、≤ 4096，奇数向下对齐为偶数；vBitrate = 0 保持当前码率。
    // 失败：TGErr_InvalidArgument（尺寸 / 码率非法、档位名不认识）/ TGErr_NotConfigured（未 SetupVideoEncoder）
    //       TGErr_NotSupported（GPU 性能模式下编码器正走硬件帧直通：编码尺寸跟随采集，不能单独切换）
    // 重开编码器失败时回退到原分辨率继续推流，并上报 TGErr_VideoEncodeFailed（回退也失败则视频停止出帧）
    virtual int SetVideoResolution(int width, int height, int vBitrate = 0) = 0;
    // 按档位切换：分辨率与码率取档位值，帧率保持不变。portrait = true 时宽高互换
    virtual int SetResolutionPreset(const std::string& name, bool portrait = false) = 0;

    // 运行统计（扁平 JSON）。bitrateInfo 同时给出实时值与累计值：
    //   实时（1s 窗口；写出停滞超过 2s 归 0）
    //     bitrate_bps / video_bps / audio_bps  编码载荷码率：合计 / 视频 / 音频（与编码码率设置直接可比）
    //     io_bps                               封装后实际写出速率（含 flv/ts 封装开销 ≈ 上行带宽）
    //   累计（自第一包起；Stop 后仍可读，下次 Prepare 清零）
    //     bytes / video_bytes / audio_bytes    编码载荷字节：合计 / 视频 / 音频
    //     io_bytes                             封装后写出字节（含文件头尾 ≈ 文件大小 / 上行总流量）
    //     packets                              已写出包数
    //     avg_bps / duration_ms                平均载荷码率 = bytes / duration；duration 自第一包起
    virtual std::string GetStatsInfo(StatsInfoType type) = 0;
    virtual float GetTrafficStats() = 0;      // 实时载荷码率 bps（= bitrateInfo.bitrate_bps）

    // ---------------- 外部帧注入（Android/iOS/游戏引擎） ----------------
    // 两个接口的失败码：
    //   TGErr_InvalidState    未 Start（或已 Stop）。视频注入在预览期间（StartPreview 之后）也可用
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

    // ---------------- 运行期音频源（任意状态可调，推流不中断） ----------------
    // 开播中增加：该路立即打开 / 开始采集，混音时淡入 20ms；移除：先淡出 20ms 再停采集、关设备。
    // 全部移除后混音输出静音，音频时间轴照常推进（视频不受影响）。
    // 旧接口创建的源也有 id：EnableMicrophoneCapture → "mic"，EnableSpeakerCapture → "speaker"，
    // PushExternalAudioFrame(type) → "ext.<type>"（仅推流期间存在，Stop 时移除）。
    //
    // AddAudioSource 失败：TGErr_InvalidArgument（id 重复 / 非法，volume 越界；File：缺路径 / 打不开 / 没有音轨）
    //                     TGErr_NotSupported（本平台无该类原生采集；File：音轨编码不支持，目前只支持 AAC）
    //                     TGErr_InvalidState（推流中但未启用音频）
    // 成功后发 EsCap_Notify_AudioSource_Added；outId 返回实际 id
    virtual int AddAudioSource(const AudioSourceDesc& desc, std::string* outId = nullptr) = 0;
    // 失败：TGErr_InvalidArgument（id 不存在）。成功后发 EsCap_Notify_AudioSource_Removed
    virtual int RemoveAudioSource(const std::string& id) = 0;
    // 线性增益 0 ~ 4，立即生效。失败：TGErr_InvalidArgument
    virtual int SetAudioSourceVolume(const std::string& id, float volume) = 0;
    // 各路电平（增益后、实际混入的样本）+ "master"（混音输出）。未开播时全部为 -100
    virtual int GetAudioLevels(std::vector<AudioSourceLevel>& levels) = 0;
    // External 类音频源推 PCM（交错 S16）。ptsUs 约定同 PushExternalAudioFrame。
    // 失败：TGErr_InvalidArgument（id 不存在 / 不是 External / 参数非法）/ TGErr_InvalidState（未开播）
    virtual int PushAudioSourceFrame(const std::string& id, const uint8_t* data, int size, int sampleRate,
                                     int channels, int64_t ptsUs) = 0;

    // ---------------- 视频叠加层：暂存 / 提交（任意状态可调，推流不中断） ----------------
    // 修改先进暂存区，CommitOverlays 时一次性原子生效：推流画面在同一帧里从旧场景切到新场景，
    // 不会出现"改了一半"的中间画面（例如同时换掉三张图、挪动画中画）。
    // 预览画面显示暂存场景（未提交的修改在预览里先看到，确认后再提交）；推流画面只含已提交的。
    // 场景跨 Prepare / Stop 保留，直到移除或销毁。视频层在暂存时就开始解码播放（预览可见），
    // 提交只是让推流画面也用上它；只修改位置 / 透明度 / 层级 / 可见性时沿用原内容，视频不会从头播。
    //
    // StageOverlay：新增，或整体替换同 id 的层。失败：
    //   TGErr_InvalidArgument（id 非法 / 几何或透明度越界 / 路径为空 / 文件打不开 / 图片解不出 / 没有视频轨）
    //   TGErr_NotSupported（视频轨的解码器未编进，目前只支持 H.264）
    virtual int StageOverlay(const OverlayDesc& desc) = 0;
    // 失败：TGErr_InvalidArgument（暂存区里没有该 id）
    virtual int StageRemoveOverlay(const std::string& id) = 0;
    // 暂存区 → 推流画面。没有改动时直接返回 0；有改动时生效后发 EsCap_Notify_Overlay_Committed
    virtual int CommitOverlays() = 0;
    // 丢弃未提交的修改（暂存区回到已提交的状态）
    virtual int DiscardOverlayChanges() = 0;
    // 当前的叠加层（staged = true 取暂存区，否则取已提交的），按绘制顺序（下层在前）
    virtual int GetOverlays(std::vector<OverlayDesc>& overlays, bool staged = false) = 0;
};

}  // namespace TGCapture

#endif  // TGCAPTURE_ITGCAPTURER_H
