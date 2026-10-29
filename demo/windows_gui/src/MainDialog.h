// 主界面：Win32 原生对话框（模板见 res/tg_capture_gui.rc）。
// 只负责"控件 ↔ LiveConfig"转换和界面状态，SDK 调用全部交给 LiveSession；预览画面交给 PreviewView。
#ifndef TGUI_MAIN_DIALOG_H
#define TGUI_MAIN_DIALOG_H

#include <windows.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "Devices.h"
#include "LiveSession.h"
#include "PreviewView.h"

namespace tgui
{

class MainDialog
{
public:
    // 初始化 SDK → 弹出主界面（模态）→ 退出时停播并反初始化。返回进程退出码
    int Run(HINSTANCE instance);

private:
    struct ResolutionPreset
    {
        std::string name;     // "720p"
        int width = 0;
        int height = 0;
        int fps = 0;
        int bitrateKbps = 0;
        std::wstring label;   // "720p   1280x720  30fps  3000kbps"
    };

    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // ---- 消息处理 ----
    void OnInitDialog();
    void OnCommand(int id, int code);
    void OnSessionEvent(std::unique_ptr<SessionEvent> ev);
    void OnStateChanged(SessionState state, bool preview);
    void OnTimer(UINT_PTR id);
    void OnSize(int width, int height);
    void OnVolumeScroll(HWND trackbar);
    void OnNotify(const NMHDR& hdr);
    void OnClose();

    // ---- 选项与设备 ----
    void FillStaticOptions();
    void FillCaptureModes();
    void FillResolutionPresets();
    void RefreshMonitors();
    void RefreshWindows();
    void RefreshAudioDevices();
    void MarkUnavailableEncoders(const std::string& video, const std::string& audio);

    // ---- 操作 ----
    void OnStartStop();
    void OnPreviewToggle();
    // forStart = false（开预览）时不校验 / 不展开输出地址；已有实例时不校验视频源（沿用实例上的）
    bool CollectConfig(LiveConfig& cfg, bool forStart);
    bool CollectAudioSlot(AudioSlot slot, AudioSlotConfig& out, bool quiet);
    void OnAudioSlotToggled(AudioSlot slot);
    void OnResolutionSelected();
    void OnSwitchResolution();
    void OnWatermark(int op);
    void OnApplyPausePicture();
    void BrowseOutput();
    void BrowseFile(int editId, const wchar_t* filter);
    void OpenOutputDir();

    // ---- 叠加层 ----
    void InitOverlayList();
    bool CollectOverlay(TGCapture::OverlayDesc& desc);
    void OnStageOverlay();
    void OnRemoveOverlay();
    void LoadOverlayForm(const TGCapture::OverlayDesc& desc);
    void RefreshOverlayList();

    // ---- 界面状态 ----
    void ApplyUiState();
    void UpdateStatsText();
    void SetLevel(int barId, float peakDb);
    void ResetLevels();
    VideoSourceKind SourceKind() const;
    bool HasCapturer() const { return m_state == SessionState::Previewing || m_state == SessionState::Live; }

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
    bool ReadInt(int id, int& out) const;   // 非空且整段是整数（允许负号）才返回 true
    int SliderPos(int id) const;
    void Enable(int id, bool on) { EnableWindow(Item(id), on ? TRUE : FALSE); }
    void Enable(std::initializer_list<int> ids, bool on);
    void Show(std::initializer_list<int> ids, bool on);
    std::wstring ResolvePath(const std::wstring& text) const;

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    PreviewView m_preview;                     // 必须比 m_session 活得久：预览回调引用它
    std::unique_ptr<LiveSession> m_session;
    SessionState m_state = SessionState::Idle;
    bool m_previewOn = false;
    bool m_pending = false;                    // 已投递开播 / 停播 / 预览请求，等工作线程回话期间按钮置灰
    bool m_liveHasAudio = false;               // 本轮推流是否带音轨（不带时直播中不能再开音频源）

    std::wstring m_exeDir;
    std::wstring m_logDir;
    std::wstring m_iniPath;
    std::wstring m_lastOutput;   // 最近一次开播的实际输出（已展开 {time}）

    std::vector<MonitorInfo> m_monitors;
    std::vector<WindowInfo> m_windows;
    std::vector<AudioDevice> m_mics;
    std::vector<AudioDevice> m_speakers;
    std::vector<bool> m_videoCodecAvailable;   // 与视频编码器下拉框一一对应；探测完成前全部视为可用
    std::vector<ResolutionPreset> m_presets;   // 与分辨率下拉框一一对应（ITGCapturerMgr::GetResolutionPresets）

    // 叠加层：最近一次从 SDK 取回的两份列表，以及列表行 → id
    std::vector<TGCapture::OverlayDesc> m_ovlCommitted;
    std::vector<TGCapture::OverlayDesc> m_ovlStaged;
    std::vector<std::string> m_ovlRows;
    bool m_fillingOverlayList = false;

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
