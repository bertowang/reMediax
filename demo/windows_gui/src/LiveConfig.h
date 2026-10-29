// 一次开播的完整配置：界面收集 → LiveSession 执行。与控件解耦，可单独阅读，也可直接复用到别的宿主。
#ifndef TGUI_LIVE_CONFIG_H
#define TGUI_LIVE_CONFIG_H

#include <windows.h>

#include <string>
#include <string_view>

#include "TGCapture/TGCapturerInterface.h"

namespace tgui
{

enum class VideoSourceKind
{
    Monitor,   // 整个显示器（CreateScreenCapture）
    Window,    // 指定窗口（CreateWindowCapture）
    File,      // 本地视频文件（CreateLocalFileCapture）
};

// 界面上的三路音频源。运行期（预览 / 直播中）可以随时开关，对应 AddAudioSource / RemoveAudioSource
enum class AudioSlot : int
{
    Mic = 0,
    Speaker = 1,
    File = 2,
};
constexpr int kAudioSlotCount = 3;

struct AudioSlotConfig
{
    bool enabled = false;
    std::wstring deviceId;   // Mic / Speaker：WASAPI 端点 id，空 = 系统默认设备
    std::string filePath;    // File：mp4 / m4a / flv 里的 AAC 音轨（UTF-8 绝对路径）
    bool loop = true;        // File：播完从头再来
    float volume = 1.0f;     // 线性增益，界面上 0 ~ 200%
    bool muted = false;      // Mic / Speaker：MuteAudio 按类型静音
};

struct LiveConfig
{
    // ---- 视频源 ----
    VideoSourceKind source = VideoSourceKind::Monitor;
    int monitorIndex = 0;         // EnumDisplayMonitors 的顺序
    HWND window = nullptr;
    TGCapture::VideoCapMode capMode = TGCapture::DXGI;   // 显示器：DXGI / GDI；窗口：WGC / WGC_No_TBar
    TGCapture::PerformanceMode perfMode = TGCapture::PerformanceMode::Cpu;
    std::string videoFile;        // 本地视频文件（UTF-8 绝对路径）
    bool videoFileLoop = true;
    bool videoFileAudio = false;  // 把视频文件自己的音轨也加进来（id = "vfile"）

    // ---- 音频源（下标 = AudioSlot）----
    AudioSlotConfig audio[kAudioSlotCount];

    // ---- 视频编码 ----
    std::string resolutionPreset = "720p";   // 档位名（仅用于日志；宽高 / 码率已按档位填好）
    int width = 1280;
    int height = 720;
    int fps = 30;
    int videoBitrateKbps = 3000;
    int gop = 60;
    std::string videoCodec = "libopenh264";
    int rotation = 0;             // 0 / 90 / 180 / 270
    bool flipH = false;
    bool flipV = false;

    // ---- 音频编码 ----
    std::string audioCodec = "libfdk_aac";
    int audioBitrateKbps = 128;

    // ---- 输出 ----
    std::string output;           // 已解析：本地文件为绝对路径（UTF-8），推流为 URL
    std::string muxer;            // flv / mpegts（"自动"已在界面侧按地址推断好）

    // ---- 运行期 ----
    int audioTimeoutMs = 3000;    // SetAVTimeout：超过该时长没有新数据即上报 *CaptureStalled
    int videoTimeoutMs = 3000;
    std::string pausePicture;     // "暂停画面"期间显示的图片；空 = 冻结最后一帧

    const AudioSlotConfig& Slot(AudioSlot s) const { return audio[static_cast<int>(s)]; }
    AudioSlotConfig& Slot(AudioSlot s) { return audio[static_cast<int>(s)]; }

    // 推流是否带音轨：开播时没有任何音频源 = 纯视频推流（之后也不能再加音频源）
    bool HasAudio() const
    {
        for (const auto& a : audio) {
            if (a.enabled) {
                return true;
            }
        }
        return source == VideoSourceKind::File && videoFileAudio;
    }
};

// rtmp:// srt:// udp:// 等带协议头的地址视为网络推流，其余视为本地文件
bool IsNetworkUrl(std::wstring_view url);

// 按地址推断封装格式：.ts / srt:// / udp:// → mpegts，其余 → flv
std::string GuessMuxer(std::wstring_view url);

// 输出地址展开：把 {time} 换成当前时间（避免反复录制互相覆盖）；
// 本地文件的相对路径以 baseDir（exe 所在目录）为基准转成绝对路径。网络地址原样返回。
std::wstring ExpandOutput(const std::wstring& text, const std::wstring& baseDir);

}  // namespace tgui

#endif  // TGUI_LIVE_CONFIG_H
