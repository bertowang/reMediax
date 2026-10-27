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
    EsCap_Notify_VideoMetaInfo_Changed = 1,
    EsCap_Notify_AudioMetaInfo_Changed = 10,
    EsCap_Notify_SystemFreeze = 20,
    EsCap_Notify_LocalFile_EOF = 21,
};

// OnErrorCallback 的 what 取值。
// 负数；Prepare 阶段的错误同时就是 Prepare() 的返回值（除 NotConfigured 外，见各项注释）。
// 回调在内核的事件分发线程上触发（Prepare 阶段的错误除外：在调用 Prepare 的线程上同步触发），
// 回调里允许调用 Stop() / 销毁对象。
enum TGCaptureError : int32_t
{
    TGErr_NotConfigured = -1,         // 编码器 / 封装器参数没有设置全（Prepare 返回 -2）
    TGErr_OpenVideoEncoder = -3,      // 视频编码器打开失败（Prepare 返回 -3）
    TGErr_OpenAudioEncoder = -4,      // 音频编码器打开失败（Prepare 返回 -4）
    TGErr_OpenMuxer = -5,             // 封装器 / 推流地址打开失败（Prepare 返回 -5）
    // ---- 运行期（Start 之后）----
    TGErr_MuxWriteFailed = -101,      // 写包持续失败超过阈值：推流断开 / 网络阻塞 / 磁盘写满。
                                      // extra = 持续失败的毫秒数。
                                      // 内核不会自动重连：FFmpeg 的 I/O 错误是粘性的，出错后同一个连接
                                      // 不会再写成功。宿主应 Stop() 后重新 Prepare/Start（Stop 不会被卡住）
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

}  // namespace TGCapture

#endif  // TGCAPTURE_TGCAPTURER_INTERFACE_H
