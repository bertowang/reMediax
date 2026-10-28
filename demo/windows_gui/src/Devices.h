// 本机设备枚举：显示器 / 可采集窗口 / 音频端点（只给界面列选项用，与 SDK 无耦合）
#ifndef TGUI_DEVICES_H
#define TGUI_DEVICES_H

#include <windows.h>

#include <string>
#include <vector>

namespace tgui
{

struct MonitorInfo
{
    std::wstring label;   // "显示器 1  2560x1440（主）"
};

struct WindowInfo
{
    HWND hwnd = nullptr;
    std::wstring label;   // "标题  —  进程名.exe"
};

struct AudioDevice
{
    std::wstring id;      // WASAPI 端点 id（传给 WinAudioCapture::SetDeviceId）
    std::wstring name;    // 友好名称
    bool isDefault = false;
};

// 顺序与 EnumDisplayMonitors 一致，下标即 CreateScreenCapture 的 index
std::vector<MonitorInfo> EnumerateMonitors();

// 与 Alt+Tab 列表同口径：可见、有标题、顶层、未被系统隐藏；exclude 一般传本工具自己的窗口
std::vector<WindowInfo> EnumerateWindows(HWND exclude);

// capture=true 麦克风，false 扬声器（系统声音 loopback）。调用线程需已初始化 COM
std::vector<AudioDevice> EnumerateAudioDevices(bool capture);

}  // namespace tgui

#endif  // TGUI_DEVICES_H
