// TGCapture 开播 SDK —— 对外接口定义（与既有 ESMediaX::ESCapturer 接口保持一致）
#ifndef TGCAPTURE_TGCAPTURER_INTERFACE_H
#define TGCAPTURE_TGCAPTURER_INTERFACE_H

#include <cstdint>
#include <string>

namespace TGCapture
{

// ------------------------------------------------------------------ 枚举
enum class GameID : int32_t
{
    GAME_ID_DEFAULT = 0,
    GAME_ID_Wangzhe_OSGame_iOS_OpenGL = 11,
    GAME_ID_Wangzhe_OSGame_iOS_Metal = 12,
    GAME_ID_Wangzhe_OSGame_iOS_Replaykit = 13,
    GAME_ID_Wangzhe_OSGame_Android_OpenGL = 14,
    GAME_ID_Wangzhe_OSGame_Android_Vulkan = 15,
    GAME_ID_Anqu_iOS_OpenGL = 21,
    GAME_ID_Anqu_iOS_Metal = 22,
    GAME_ID_Anqu_iOS_Replaykit = 23,
    GAME_ID_Anqu_Android_OpenGL = 24,
    GAME_ID_Anqu_Android_Vulkan = 25,
};

enum AudioCapSourceType
{
    TypeOfANone = -1,
    TypeOfGameAudio4Slave = 0,      // 游戏音频（外部注入）
    TypeOfGameMicphone4Slave = 1,   // 游戏麦克风（外部注入）
    TypeOfGameTeamVoice4Slave = 2,  // 组队语音（外部注入）
    TypeOfSelfMicphone = 10,        // PC 麦克风
    TypeOfGVoiceMicphone = 11,      // PC GVoice 麦克风
    TypeOfSpeaker = 20,             // PC 扬声器
    TypeOfLocalFileAudio = 30,      // 本地媒体文件的音轨（AudioSourceKind::File）
    TypeOfArtificialAudio = 100,    // 人造静音音频（测试）
};

enum VideoCapSourceType
{
    TypeOfVNone = -1,
    TypeOfGameEngine4Slave = 0,  // 被动等待上层 push 数据
    TypeOfSelfScreen = 10,
    TypeOfWindow = 11,
    TypeOfSelfCamera = 12,
    TypeOfLocalFile = 20,
};

enum NotifyMsgType
{
    EsCap_Notify_Fisrt_Frame_OK = 0,
    // 编码分辨率 / 码率已切换（SetVideoResolution / SetResolutionPreset 在推流中生效后发出，
    // 新参数的第一个 IDR 已进入封装）：content = "宽x高"，data = 目标码率 bps
    EsCap_Notify_VideoMetaInfo_Changed = 1,
    EsCap_Notify_AudioMetaInfo_Changed = 10,
    EsCap_Notify_SystemFreeze = 20,
    // 文件源（loop = false）播放结束。content 区分是哪个源：
    //   音频源 id（AudioSourceKind::File）/ "video:main"（CreateLocalFileCapture 的主画面）/ "overlay:<id>"（视频叠加层）
    EsCap_Notify_LocalFile_EOF = 21,
    // 断流恢复：与 TGErr_VideoCaptureStalled / TGErr_AudioCaptureStalled 配对，
    // 收到它表示该路又有数据了（data = 本次断流持续的毫秒数）
    EsCap_Notify_VideoCapture_Recovered = 30,
    EsCap_Notify_AudioCapture_Recovered = 31,
    // 编码器回落（Prepare 期间发出）：请求的编码器本机打不开，已自动换成兜底编码器。
    // content = "video:h264_mediacodec->libopenh264" 或 "audio:aac_at->aac"；data = 0 视频 / 1 音频。
    // 想提前知道哪些编码器可用，用 ITGCapturerMgr::IsEncoderAvailable / GetAvailableEncoders。
    EsCap_Notify_Encoder_Fallback = 40,
    // 运行期音频源增删（AddAudioSource / RemoveAudioSource 成功后发出）：content = 音频源 id，data = 混流通道号
    EsCap_Notify_AudioSource_Added = 41,
    EsCap_Notify_AudioSource_Removed = 42,
    // 叠加层场景已提交（CommitOverlays 有改动时发出，推流画面已切到新场景）：data = 提交后的叠加层数
    EsCap_Notify_Overlay_Committed = 43,
};

// ------------------------------------------------------------------ 错误码
// 全工程唯一的错误码空间：**接口返回值与 OnErrorCallback 的 what 取同一套码**，
// 同一个数值在任何位置都只有一个含义（这是本枚举存在的首要目的）。
//
// 约定
//   1. 0 = 成功，负数 = 失败；返回值为负时一定能在本枚举里查到。
//   2. extra = **底层原始错误码**（FFmpeg 的 AVERROR、平台 API 的返回码、子模块码），
//      仅用于排障，不要用它做分支判断；没有底层码时为 0。
//      唯一的例外在各项注释里显式写明。
//   3. 分三段，便于宿主按段粗分类：
//        -1  ~ -19   调用级：状态 / 配置缺失 / 打开失败（Prepare、Start 的返回值）
//        -20 ~ -99   调用级：参数与能力（配置类接口、外部注入接口的返回值）
//        -100 以下   运行期：Start 之后由内核主动上报，只经 OnErrorCallback，无返回值对应
//   4. 回调线程：Prepare / Start 的失败在调用方线程上同步触发；运行期错误在内核的
//      事件分发线程上触发。两者都不持有内部锁，回调里允许调用 Stop() / 销毁对象。
enum TGCaptureError : int32_t
{
    TGErr_OK = 0,

    // ---- 调用级：状态与打开失败（Prepare / Start 的返回值，同时经 OnError 上报）----
    TGErr_InvalidState = -1,       // 当前状态不允许该操作：重复 Prepare、未 Prepare 就 Start、
                                   // 未 Start 就 PushExternal*Frame。extra = 当前内部状态值
    TGErr_NotConfigured = -2,      // SetupVideoEncoder / SetupAudioEncoder / SetupMuxer 没有全部成功调用过
    TGErr_OpenVideoEncoder = -3,   // 视频编码器打开失败。extra = AVERROR 或子模块码
    TGErr_OpenAudioEncoder = -4,   // 音频编码器打开失败。extra = AVERROR 或子模块码
    TGErr_OpenMuxer = -5,          // 封装器 / 推流地址打开失败（含连接超时）。extra = AVERROR
    TGErr_OpenVideoSource = -6,    // 已配置的**原生**视频采集源打不开（设备被占用 / 权限未授予 / 无桌面复制通道）。
                                   // 外部注入源（TypeOfGameEngine4Slave）打开失败不算错误，由上层推帧
    TGErr_StartFailed = -7,        // 内部线程启动失败（上一轮未停止干净）。extra = 失败的环节：
                                   // 1=muxer 2=video encoder 3=audio encoder

    // ---- 调用级：参数与能力 ----
    TGErr_InvalidArgument = -20,   // 入参非法：空指针、尺寸 ≤ 0、stride < width、采样率 / 声道数 ≤ 0、
                                   // 推流地址为空、源对象类型不匹配
    TGErr_NotSupported = -21,      // 当前平台 / 当前模式不支持：Linux 原生采集、非 Win/iOS 的外部硬件帧
    TGErr_NotImplemented = -22,    // 接口占位，尚未实现
    TGErr_Internal = -23,          // 内部资源创建失败（混流通道、静帧加载、水印纹理等）

    // ---- 运行期（Start 之后，只经 OnErrorCallback）----
    TGErr_MuxWriteFailed = -101,       // 写包持续失败超过 MuxerParams::writeFailReportMs：
                                       // 推流断开 / 网络阻塞 / 磁盘写满。extra = 最后一次的 AVERROR。
                                       // **内核不会自动重连**：FFmpeg 的 I/O 错误是粘性的，出错后同一个
                                       // 连接不会再写成功。宿主应 Stop() 后重新 Prepare/Start（Stop 不会被卡住）
    TGErr_VideoEncodeFailed = -102,    // 视频编码持续失败超过 1s（编码器内部错误 / 硬编设备丢失）。extra = AVERROR
    TGErr_AudioEncodeFailed = -103,    // 音频编码持续失败超过 1s。extra = AVERROR
    TGErr_VideoCaptureStalled = -104,  // 视频超过 SetAVTimeout 的 videoTimeoutMs 没有新帧：
                                       // 采集设备丢失 / 权限被撤销 / 屏幕采集重建失败 / 上层停止推帧。
                                       // extra = 已断流毫秒数。恢复时发 EsCap_Notify_VideoCapture_Recovered
    TGErr_AudioCaptureStalled = -105,  // 音频超过 audioTimeoutMs 没有新数据（含"采集源打开失败后静音兜底"）。
                                       // extra = 已断流毫秒数。恢复时发 EsCap_Notify_AudioCapture_Recovered
    TGErr_MuxTrailerFailed = -106,     // 收尾写文件尾 / 关闭连接失败：产出文件可能缺索引、时长不对。
                                       // extra = AVERROR。出现在 Stop 过程中，紧随其后的 OnStop 会带同一个码
};

enum VideoCapMode
{
    NONE = -1,
    DXGI = 0,   // Windows 桌面复制（Desktop Duplication，性能最好）
    D3D = 1,
    GDI = 2,    // Windows GDI（兼容性兜底，DXGI 不可用/多显示器/远程桌面时回落）
    WGC = 4,    // Windows 窗口采集（含边框）
    WGC_No_TBar = 5,  // Windows 窗口采集（仅客户区，去标题栏/边框）
    AndroidSurface = 10,   // Android MediaProjection（虚拟显示 -> Surface / ImageReader）
    ReplayKit = 11,        // iOS ReplayKit（应用内录屏）
    LocalVideoFile = 20,
};

// 采集能力查询：三端能力不对齐（iOS 无"窗口"概念、无系统内录；
// Android 无"窗口"概念），调用方在 Start 之前用 ITGCapturer::IsSupported 判断。
enum class CaptureFeature
{
    Screen = 0,      // 屏幕录制
    Window = 1,      // 指定窗口采集
    Microphone = 2,  // 麦克风
    Speaker = 3,     // 扬声器 / 系统声音
};

// 性能模式：Cpu = 传统 CPU 链路（采集→内存→软编/硬编）；
// Gpu  = 全链路 GPU：采集帧留在显存/平台硬件缓冲里，直接喂硬编，CPU 不做像素搬运。
// Gpu 不可用时（无硬编、无平台 API、采集源不支持）会自动回落 Cpu 并打日志。
enum class PerformanceMode
{
    Cpu = 0,
    Gpu = 1,
};

enum class VideoScaleStrategy
{
    SCALE_FIT_V0 = 0,  // 保持宽高比 + 黑边（默认）
    SCALE_FIT = 1,
    SCALE_CROP = 10,
};

enum AudioEncoder
{
    libfdk_aac = 0,
    aac = 1,
    aac_at = 2,
};

// 注意：沿用既有拼写（Encdoer），保证老调用方编译通过
enum VideoEncdoer
{
    libopenh264 = 0,
    h264_nvenc = 1,
    h264_mediacodec = 2,
    h264_videotoolbox = 3,
};

enum OutputFormat
{
    FLV = 0,
    MPEGTS = 1,
};

enum class StatsInfoType
{
    none = 0,
    metaInfo = 1,
    bitrateInfo = 2,
    pktInfo = 3,
    audioInfo = 4,
    videoInfo = 5,
};

enum class LogoOpType
{
    LOGO_INIT = -1,
    LOGO_RM = 0,
    LOGO_ADD = 1,
    LOGO_UPDATE = 2,
};

// ------------------------------------------------------------------ 源
class IAudioSource
{
public:
    explicit IAudioSource(AudioCapSourceType type) : m_type(type) {}
    virtual ~IAudioSource() = default;
    AudioCapSourceType GetAudioCapSourceType() const noexcept { return m_type; }

private:
    AudioCapSourceType m_type = TypeOfANone;
};

class IVideoSource
{
public:
    IVideoSource(VideoCapSourceType type, VideoScaleStrategy strategy = VideoScaleStrategy::SCALE_FIT_V0)
        : m_type(type), m_strategy(strategy) {}
    virtual ~IVideoSource() = default;

    VideoCapSourceType GetVideoCapSourceType() const noexcept { return m_type; }
    VideoScaleStrategy GetScaleStrategy() const noexcept { return m_strategy; }
    void SetScaleStrategy(VideoScaleStrategy strategy) noexcept { m_strategy = strategy; }

private:
    VideoCapSourceType m_type = TypeOfVNone;
    VideoScaleStrategy m_strategy = VideoScaleStrategy::SCALE_FIT_V0;
};

// ------------------------------------------------------------------ 参数
struct VideoEncoderParams
{
    std::string ConfigureVersion;
    // 分辨率档位名（"720p" 等），由 ITGCapturerMgr::ApplyResolutionPreset 填写，仅用于日志 / 统计
    std::string ResolutionName;

    int width = 1280;
    int height = 720;
    float fps = 30.0f;
    int vScale = 512;
    int gop = 15;
    std::string vCodec = "libopenh264";   // 以字符串为准（h264_amf/h264_mf 等不在枚举里）
    VideoEncdoer vEncoderType = libopenh264;

    int threads = 4;
    int vBitrate = 3000000;
    float vBitrateMaxFactor = 3.0f;
    std::string vExtParams;

    int vConverterType = -1;
    bool bUseAndroidSurface = false;      // Android Surface 直连硬编

    // ---- 性能模式 ----
    // Gpu：采集帧留在硬件缓冲，直送硬编（Windows: D3D11 纹理→NVENC/AMF/MF；
    // iOS: CVPixelBuffer→VideoToolbox；Android: 编码器 Surface→MediaCodec）。
    // 不可用时自动回落 Cpu；想强制 Cpu 就显式设为 Cpu。
    PerformanceMode perfMode = PerformanceMode::Cpu;

    // ---- 画面变换（采集帧 → 编码分辨率之前统一应用）----
    // 顺序固定为 crop → 翻转 → 旋转 → 缩放 → 像素格式转换。
    // 全部留默认 = 不做变换，走原有的 sws_scale 路径（零额外开销）。
    int videoRotation = 0;             // 顺时针，只取 0 / 90 / 180 / 270
    bool videoFlipHorizontal = false;  // 水平镜像
    bool videoFlipVertical = false;    // 垂直翻转
    int videoCropX = 0;                // 裁剪原点（源帧坐标系）
    int videoCropY = 0;
    int videoCropWidth = 0;            // <=0 表示不裁剪
    int videoCropHeight = 0;

    // GPU 模式下，若需要变换或采集分辨率与编码分辨率不一致，
    // 是否允许把纹理回读到内存用 libyuv 处理（否则该帧直接丢弃）。
    // 关掉可保证 GPU 链路"绝不出显存"，代价是这类帧会被跳过。
    bool gpuAllowReadbackFallback = true;

    // 外部注入（PushExternalVideoFrame）时是否沿用调用方传入的 ptsUs。
    // 默认 false：视频 PTS 由音频主时钟反推 —— 这是开播场景的正确做法，
    // 传入的 ptsUs 只在"无音频 / 音频未就绪"时作为兜底。
    // 只有调用方自带完整时间轴（如游戏引擎已做过音画同步）才需要置 true；
    // 置 true 后内核不再改写 PTS，音画同步的责任转移到调用方。
    bool videoUseExternalPts = false;

    // ---- 音画同步 ----
    int64_t videoMaxLeadUs = 500000;   // 音频停滞时视频最多超前多少（默认 500ms）
    // 视频落后音频超过多少就丢帧（编码器侧最后一道兜底）。
    // 0 = 自动：由音画同步模块按 forceSyncFactor × 帧间隔 × 1.5 推导，
    //           保证阈值落在 RangeAdapt 追赶区间之外，不与同步策略互相抵消。
    // 显式给非 0 值时以宿主配置为准。
    int64_t videoMaxLagUs = 0;
    int64_t audioCompensationUs = 0;   // 音频参考时钟的额外补偿（抵消编码/封装延迟）
};

struct AudioEncoderParams
{
    int sampleRate = 44100;
    int channels = 2;
    int aBitrate = 128000;
    std::string aCodec = "libfdk_aac";
    std::string aExtParams;
    // false = 纯视频推流：不创建音频编码器、封装器也不建音频流（默认 true，老调用方无感）
    bool enabled = true;
};

struct MuxerParams
{
    std::string muxer = "flv";        // flv / mpegts
    std::string muxerExtParams;
    std::string muxerOutput;          // rtmp:// srt:// 或文件路径
    // 音视频交织阈值：领先的一方暂停写入，等落后的一方追上（0 = 关闭）
    int interleaveMs = 350;
    // 连接 / 握手阶段（Prepare 里打开输出）的最长等待。超时 Prepare 返回 -5，不会一直卡住。0 = 用默认 10000
    int connectTimeoutMs = 10000;
    // 运行期单次写包的最长阻塞。网络卡死时超时打断，Stop 不会被卡住。0 = 用默认 5000
    int ioTimeoutMs = 5000;
    // 写包持续失败多久后上报 TGErr_MuxWriteFailed。0 = 用默认 3000
    int writeFailReportMs = 3000;
};

struct PauseParams
{
    std::string picture;  // 空=冻结最后一帧；有路径=用该图片
    int rotate = 0;
};

struct WatermarkParams
{
    uint32_t index = 0;
    LogoOpType type = LogoOpType::LOGO_INIT;
    int32_t position = -1;  // -1:用 xy；0左上 1右上 2左下 3右下 4顶中 5底中
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    std::string url;
};

// ------------------------------------------------------------------ 运行期音频源
enum class AudioSourceKind : int32_t
{
    Microphone = 0,   // 系统麦克风（WASAPI / AudioRecord / AVAudioSession）
    Speaker = 1,      // 系统内录（Windows WASAPI loopback / Android AudioPlaybackCapture，需 MediaProjection）
    External = 2,     // 宿主推 PCM（PushAudioSourceFrame）
    File = 3,         // 本地媒体文件的音轨（mp4 / mov / m4a / flv 里的 AAC），内核解码、按实时节奏混入；
                      // loop = false 播完发 EsCap_Notify_LocalFile_EOF（content = id），之后该路输出静音
};

struct AudioSourceDesc
{
    AudioSourceKind kind = AudioSourceKind::External;
    std::string id;             // 唯一标识（字母数字 . _ -）；空 = 内核生成：mic / speaker / pcm / file（重名加 .2 .3 …）
    float volume = 1.0f;        // 线性增益 0 ~ 4
    std::string filePath;       // File
    bool loop = true;           // File：播完从头再来
};

struct AudioSourceLevel
{
    std::string id;             // 音频源 id；"master" = 混音输出
    float peakDb = -100.0f;     // dBFS，-100 = 静音
    float rmsDb = -100.0f;
};

// ------------------------------------------------------------------ 视频叠加层
enum class OverlayKind : int32_t
{
    Image = 0,       // 静态图片（png，带透明通道则按透明度混合）
    VideoFile = 1,   // 本地视频文件（mp4 / mov / flv 里的 H.264），按实时节奏播放；画中画
};

// 叠加层：画在主画面（含暂停图）之上、水印之下，仅对 CPU 帧生效（GPU 硬件帧直通时跳过）。
// 位置 / 大小用相对画布的比例描述，按每帧的实际尺寸换算成像素 —— 运行期切分辨率后自动跟随
struct OverlayDesc
{
    std::string id;             // 唯一标识（字母数字 . _ -），必填
    OverlayKind kind = OverlayKind::Image;
    std::string path;
    bool loop = true;           // VideoFile：播完从头再来；false = 播完停在最后一帧，发 EsCap_Notify_LocalFile_EOF
    float x = 0.0f;             // 左上角，画布宽 / 高的比例，-1 ~ 1（允许部分移出画面）
    float y = 0.0f;
    float width = 0.25f;        // 画布宽的比例，(0, 2]
    float height = 0.0f;        // 画布高的比例，[0, 2]；0 = 按素材宽高比由 width 推出
    float opacity = 1.0f;       // 0 ~ 1
    int32_t zOrder = 0;         // 大的在上；相同时按 id 字典序
    bool visible = true;        // false = 保留该层但不画（视频层照常播放）
};

// ------------------------------------------------------------------ 预览
enum class PreviewPixelFormat : int32_t
{
    NV12 = 0,       // y / uv 两平面有效
    Hardware = 1,   // hwHandle 有效：iOS CVPixelBufferRef / Windows ID3D11Texture2D*（仅 GPU 性能模式）
};

// 预览帧：与推流画面同源（含暂停图 / 水印 / 叠加层），未经编码前的缩放与变换。
// 叠加层按**暂存场景**绘制：有未提交的修改时，预览先看到、推流画面要等 CommitOverlays 才变（见 StageOverlay）。
// 指针只在回调期间有效；宿主需要保留就自行拷贝
struct PreviewFrame
{
    PreviewPixelFormat format = PreviewPixelFormat::NV12;
    int width = 0;
    int height = 0;
    const uint8_t* y = nullptr;
    int yStride = 0;
    const uint8_t* uv = nullptr;
    int uvStride = 0;
    void* hwHandle = nullptr;
    int64_t captureUs = 0;   // 采集时刻（单调时钟，微秒）
};

}  // namespace TGCapture

#endif  // TGCAPTURE_TGCAPTURER_INTERFACE_H
