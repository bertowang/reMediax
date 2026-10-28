// 主界面：Win32 原生对话框（模板见 res/tg_capture_gui.rc）。
// 只负责"控件 ↔ LiveConfig"转换和界面状态，SDK 调用全部交给 LiveSession。
#ifndef TGUI_MAIN_DIALOG_H
#define TGUI_MAIN_DIALOG_H

#include <windows.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "Devices.h"
#include "LiveSession.h"

namespace tgui
{

class MainDialog
{
public:
    // 初始化 SDK → 弹出主界面（模态）→ 退出时停播并反初始化。返回进程退出码
    int Run(HINSTANCE instance);

private:
    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // ---- 消息处理 ----
    void OnInitDialog();
    void OnCommand(int id, int code);
    void OnSessionEvent(std::unique_ptr<SessionEvent> ev);
    void OnTimer(UINT_PTR id);
    void OnSize(int width, int height);
    void OnClose();

    // ---- 选项与设备 ----
    void FillStaticOptions();
    void FillCaptureModes();
    void RefreshMonitors();
    void RefreshWindows();
    void RefreshAudioDevices();
    void MarkUnavailableEncoders(const std::string& video, const std::string& audio);

    // ---- 操作 ----
    void OnStartStop();
    bool CollectConfig(LiveConfig& cfg);
    void OnWatermark(int op);
    void OnApplyPausePicture();
    void BrowseOutput();
    void BrowseImage(int editId);
    void OpenOutputDir();

    // ---- 界面状态 ----
    void ApplyUiState();
    void UpdateStatsText();

    // ---- 日志 ----
    void Log(const std::wstring& text);
    void FlushLog();

    // ---- 设置持久化（exe 同目录的 tg_capture_gui.ini）----
    void LoadSettings();
    void SaveSettings();

    // ---- 控件小工具 ----
    HWND Item(int id) const { return GetDlgItem(m_hwnd, id); }
    bool IsChecked(int id) const { return IsDlgButtonChecked(m_hwnd, id) == BST_CHECKED; }
    void SetChecked(int id, bool on) { CheckDlgButton(m_hwnd, id, on ? BST_CHECKED : BST_UNCHECKED); }
    int Sel(int id) const;
    void SetSel(int id, int index);
    std::wstring Text(int id) const;
    int IntOf(int id, int fallback) const;
    void Enable(int id, bool on) { EnableWindow(Item(id), on ? TRUE : FALSE); }
    void Enable(std::initializer_list<int> ids, bool on);
    std::wstring ResolvePath(const std::wstring& text) const;

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    std::unique_ptr<LiveSession> m_session;
    SessionState m_state = SessionState::Idle;

    std::wstring m_exeDir;
    std::wstring m_logDir;
    std::wstring m_iniPath;
    std::wstring m_lastOutput;   // 最近一次开播的实际输出（已展开 {time}）

    std::vector<MonitorInfo> m_monitors;
    std::vector<WindowInfo> m_windows;
    std::vector<AudioDevice> m_mics;
    std::vector<AudioDevice> m_speakers;
    std::vector<bool> m_videoCodecAvailable;   // 与视频编码器下拉框一一对应；探测完成前全部视为可用

    bool m_videoPaused = false;
    ULONGLONG m_liveSinceMs = 0;
    std::wstring m_lastStats;

    // 状态文字（加粗 + 按状态着色）
    HFONT m_statusFont = nullptr;
    COLORREF m_statusColor = RGB(128, 128, 128);

    // 窗口缩放：日志区随窗口拉伸
    SIZE m_baseClient{};
    POINT m_minTrack{};
    std::vector<RECT> m_anchorRects;
};

}  // namespace tgui

#endif  // TGUI_MAIN_DIALOG_H
