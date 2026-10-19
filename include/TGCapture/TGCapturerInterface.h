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

enum VideoCapMode
{
    NONE = -1,
    DXGI = 0,   // Windows 桌面复制
    D3D = 1,
    GDI = 2,    // Windows GDI（兼容性兜底）
    WGC = 4,    // Windows Graphics Capture（窗口）
    WGC_No_TBar = 5,
    AndroidSurface = 10,
    LocalVideoFile = 20,
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
};

struct AudioEncoderParams
{
    int sampleRate = 44100;
    int channels = 2;
    int aBitrate = 128000;
    std::string aCodec = "libfdk_aac";
    std::string aExtParams;
};

struct MuxerParams
{
    std::string muxer = "flv";        // flv / mpegts
    std::string muxerExtParams;
    std::string muxerOutput;          // rtmp:// srt:// 或文件路径
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
