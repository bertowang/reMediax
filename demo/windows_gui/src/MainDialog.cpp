#include "MainDialog.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string_view>

#include "LogBuffer.h"
#include "StringUtil.h"
#include "core/Logger.h"
#include "resource.h"

namespace fs = std::filesystem;

namespace tgui
{

namespace
{

using namespace TGCapture;

// ------------------------------------------------------------------ 下拉框选项（界面文案 ↔ SDK 取值）
struct IntChoice
{
    const wchar_t* label;
    int value;
};
struct TextChoice
{
    const wchar_t* label;
    const char* value;
};
struct ResChoice
{
    const wchar_t* label;
    int width;
    int height;
};

constexpr IntChoice kMonitorModes[] = {{L"DXGI 桌面复制（推荐）", DXGI}, {L"GDI（兼容兜底）", GDI}};
constexpr IntChoice kWindowModes[] = {{L"整窗（含标题栏）", WGC}, {L"仅客户区（去标题栏）", WGC_No_TBar}};
constexpr IntChoice kPerfModes[] = {{L"CPU（兼容性最好）", 0}, {L"GPU（显存直送硬编）", 1}};
constexpr TextChoice kMuxers[] = {{L"自动（按地址判断）", ""}, {L"flv", "flv"}, {L"mpegts", "mpegts"}};
constexpr ResChoice kResolutions[] = {
    {L"1920 x 1080（1080P）", 1920, 1080}, {L"1280 x 720（720P）", 1280, 720}, {L"960 x 540（540P）", 960, 540}};
constexpr IntChoice kFps[] = {{L"15 fps", 15}, {L"24 fps", 24}, {L"30 fps", 30}, {L"60 fps", 60}};
constexpr TextChoice kVideoCodecs[] = {
    {L"libopenh264（软编，任何机器可用）", "libopenh264"},
    {L"h264_nvenc（NVIDIA 硬编）", "h264_nvenc"},
    {L"h264_amf（AMD 硬编）", "h264_amf"},
    {L"h264_mf（Media Foundation 硬编）", "h264_mf"},
};
constexpr TextChoice kAudioCodecs[] = {{L"libfdk_aac", "libfdk_aac"}, {L"aac（FFmpeg 内置）", "aac"}};
constexpr IntChoice kAudioBitrates[] = {{L"64", 64}, {L"96", 96}, {L"128", 128}, {L"192", 192}};
constexpr IntChoice kRotations[] = {{L"不旋转", 0}, {L"顺时针 90°", 90}, {L"180°", 180}, {L"顺时针 270°", 270}};
constexpr IntChoice kFlips[] = {{L"不翻转", 0}, {L"水平镜像", 1}, {L"垂直翻转", 2}, {L"水平 + 垂直", 3}};
constexpr IntChoice kLogoIndexes[] = {{L"0", 0}, {L"1", 1}, {L"2", 2}, {L"3", 3}, {L"4", 4}, {L"5", 5}};
constexpr IntChoice kLogoPositions[] = {{L"左上角", 0},   {L"右上角", 1},   {L"左下角", 2},     {L"右下角", 3},
                                        {L"顶部居中", 4}, {L"底部居中", 5}, {L"自定义 X/Y", -1}};
constexpr IntChoice kLogLevels[] = {{L"Debug", 0}, {L"Info", 1}, {L"Warn", 2}, {L"Error", 3}};

// 输出地址预设：{time} 在开播时替换为当前时间，相对路径以 exe 目录为基准
constexpr const wchar_t* kOutputPresets[] = {
    L"records\\tg_{time}.flv",
    L"records\\tg_{time}.ts",
    L"rtmp://127.0.0.1/live/stream",
    L"srt://127.0.0.1:10080?streamid=#!::r=live/stream,m=publish",
};

// 默认选中项（下标）
constexpr int kDefaultResolution = 1;   // 720P
constexpr int kDefaultFps = 2;          // 30fps
constexpr int kDefaultAudioBitrate = 2; // 128kbps
constexpr int kDefaultLogoPos = 3;      // 右下角
constexpr int kDefaultLogLevel = 1;     // Info

constexpr UINT_PTR kTimerLog = 1;     // 200ms：把缓冲的日志刷到界面
constexpr UINT_PTR kTimerStats = 2;   // 1s：直播中拉一次统计

// 随窗口缩放的控件：日志区四向拉伸，状态区横向拉伸，日志按钮贴右
struct Anchor
{
    int id;
    bool moveX;
    bool growX;
    bool growY;
};
constexpr Anchor kAnchors[] = {
    {IDC_STATUS_GROUP, false, true, false}, {IDC_STATUS, false, true, false},     {IDC_STATS, false, true, false},
    {IDC_LOG_GROUP, false, true, true},     {IDC_LOG, false, true, true},         {IDC_LOG_CLEAR, true, false, false},
    {IDC_OPEN_LOG_DIR, true, false, false},
};

// 需要持久化的控件
enum class Kind
{
    Combo,
    Check,
    Text
};
struct PersistItem
{
    const wchar_t* key;
    int id;
    Kind kind;
};
constexpr PersistItem kPersist[] = {
    {L"cap_mode", IDC_CAP_MODE, Kind::Combo},       {L"perf_mode", IDC_PERF_MODE, Kind::Combo},
    {L"monitor", IDC_MONITOR_LIST, Kind::Combo},    {L"mic_enable", IDC_MIC_ENABLE, Kind::Check},
    {L"mic_mute", IDC_MIC_MUTE, Kind::Check},       {L"spk_enable", IDC_SPK_ENABLE, Kind::Check},
    {L"spk_mute", IDC_SPK_MUTE, Kind::Check},       {L"output", IDC_OUTPUT_URL, Kind::Text},
    {L"muxer", IDC_MUXER, Kind::Combo},             {L"resolution", IDC_RESOLUTION, Kind::Combo},
    {L"fps", IDC_FPS, Kind::Combo},                 {L"vcodec", IDC_VCODEC, Kind::Combo},
    {L"vbitrate", IDC_VBITRATE, Kind::Text},        {L"gop", IDC_GOP, Kind::Text},
    {L"acodec", IDC_ACODEC, Kind::Combo},           {L"abitrate", IDC_ABITRATE, Kind::Combo},
    {L"rotation", IDC_ROTATION, Kind::Combo},       {L"flip", IDC_FLIP, Kind::Combo},
    {L"audio_timeout", IDC_A_TIMEOUT, Kind::Text},  {L"video_timeout", IDC_V_TIMEOUT, Kind::Text},
    {L"pause_picture", IDC_PAUSE_PIC, Kind::Text},  {L"logo_path", IDC_LOGO_PATH, Kind::Text},
    {L"logo_index", IDC_LOGO_INDEX, Kind::Combo},   {L"logo_pos", IDC_LOGO_POS, Kind::Combo},
    {L"logo_x", IDC_LOGO_X, Kind::Text},            {L"logo_y", IDC_LOGO_Y, Kind::Text},
    {L"log_level", IDC_LOG_LEVEL, Kind::Combo},
};
constexpr const wchar_t* kIniSection = L"ui";

template <typename T, size_t N>
void FillCombo(HWND combo, const T (&items)[N], int sel)
{
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

template <typename T, size_t N>
const T& Pick(const T (&items)[N], int index)
{
    return items[(index >= 0 && static_cast<size_t>(index) < N) ? index : 0];
}

std::wstring Trim(const std::wstring& s)
{
    const size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const size_t last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path().wstring();
}

bool ListContains(const std::string& csv, const char* name)
{
    size_t pos = 0;
    while (pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        const std::string item = csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (item == name) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

const wchar_t* StateLabel(SessionState s)
{
    switch (s) {
        case SessionState::Idle: return L"● 未开播";
        case SessionState::Connecting: return L"● 正在连接 / 打开输出…";
        case SessionState::Live: return L"● 直播中";
        case SessionState::Stopping: return L"● 正在停止…";
    }
    return L"";
}

COLORREF StateColor(SessionState s)
{
    switch (s) {
        case SessionState::Live: return RGB(16, 150, 60);
        case SessionState::Connecting:
        case SessionState::Stopping: return RGB(220, 130, 0);
        default: return RGB(128, 128, 128);
    }
}

}  // namespace

// ------------------------------------------------------------------ 入口
int MainDialog::Run(HINSTANCE instance)
{
    m_instance = instance;
    m_exeDir = ExeDirectory();
    m_logDir = (fs::path(m_exeDir) / L"logs").wstring();
    m_iniPath = (fs::path(m_exeDir) / L"tg_capture_gui.ini").wstring();

    // 内核日志：落盘到 exe\logs，同时经 sink 转发到界面日志区与 VS「输出」窗口
    const int levelIndex = static_cast<int>(GetPrivateProfileIntW(kIniSection, L"log_level", kDefaultLogLevel, m_iniPath.c_str()));
    ITGCapturerMgr::Init(Pick(kLogLevels, levelIndex).value, WideToUtf8(m_logDir), /*printLog*/ false);
    core::Logger::Instance().SetSink([](core::LogLevel level, const char* msg) {
        static constexpr wchar_t kTags[] = {L'D', L'I', L'W', L'E'};
        const int i = static_cast<int>(level);
        LogBuffer::Instance().Push((i >= 0 && i < 4) ? kTags[i] : L'?', Utf8ToWide(msg ? msg : ""));
    });

    const INT_PTR ret = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, &MainDialog::DlgProc,
                                        reinterpret_cast<LPARAM>(this));

    m_session.reset();   // 正常路径已在 OnClose 里 Shutdown，这里兜底
    core::Logger::Instance().SetSink(nullptr);
    ITGCapturerMgr::UnInit();
    if (m_statusFont) {
        DeleteObject(m_statusFont);
    }
    return static_cast<int>(ret);
}

INT_PTR CALLBACK MainDialog::DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MainDialog* self = nullptr;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainDialog*>(lp);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    }
    return self ? self->HandleMessage(msg, wp, lp) : FALSE;
}

INT_PTR MainDialog::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_INITDIALOG:
            OnInitDialog();
            return TRUE;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return TRUE;
        case WM_SESSION_EVENT:
            OnSessionEvent(std::unique_ptr<SessionEvent>(reinterpret_cast<SessionEvent*>(lp)));
            return TRUE;
        case WM_TIMER:
            OnTimer(wp);
            return TRUE;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                OnSize(LOWORD(lp), HIWORD(lp));
            }
            return TRUE;
        case WM_GETMINMAXINFO:
            if (m_minTrack.x > 0) {
                reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = m_minTrack;
            }
            return TRUE;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lp) == Item(IDC_STATUS)) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, m_statusColor);
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
            }
            return FALSE;
        case WM_CLOSE:
            OnClose();
            return TRUE;
        default:
            return FALSE;
    }
}

// ------------------------------------------------------------------ 初始化
void MainDialog::OnInitDialog()
{
    SetWindowTextW(m_hwnd, (L"TGCapture 开播测试工具  v" + Utf8ToWide(ITGCapturerMgr::GetVersion())).c_str());
    HICON icon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APP));
    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));

    // 状态文字用加粗大一号的字体
    LOGFONTW lf{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(m_hwnd, WM_GETFONT, 0, 0)), sizeof(lf), &lf);
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 5 / 4;
    m_statusFont = CreateFontIndirectW(&lf);
    SendMessageW(Item(IDC_STATUS), WM_SETFONT, reinterpret_cast<WPARAM>(m_statusFont), TRUE);

    SendMessageW(Item(IDC_LOG), EM_SETLIMITTEXT, 0, 0);

    // 记录初始布局，供缩放时计算
    RECT client{};
    GetClientRect(m_hwnd, &client);
    m_baseClient = {client.right, client.bottom};
    RECT window{};
    GetWindowRect(m_hwnd, &window);
    m_minTrack = {window.right - window.left, window.bottom - window.top};
    for (const auto& a : kAnchors) {
        RECT r{};
        GetWindowRect(Item(a.id), &r);
        MapWindowPoints(nullptr, m_hwnd, reinterpret_cast<POINT*>(&r), 2);
        m_anchorRects.push_back(r);
    }

    FillStaticOptions();
    RefreshMonitors();
    RefreshAudioDevices();
    LoadSettings();

    m_session = std::make_unique<LiveSession>(m_hwnd);
    SetTimer(m_hwnd, kTimerLog, 200, nullptr);

    Log(L"界面就绪。配置好左侧选项后点「开始直播」；日志同时写入 " + m_logDir);
    Log(L"正在后台探测本机可用的编码器…");
    m_session->ProbeEncoders();

    SetDlgItemTextW(m_hwnd, IDC_STATS, L"开播后每秒刷新：码率、已写出数据量、编码帧数、丢帧、积压。");
    ApplyUiState();
}

void MainDialog::FillStaticOptions()
{
    SetChecked(IDC_SRC_MONITOR, true);
    FillCaptureModes();
    FillCombo(Item(IDC_PERF_MODE), kPerfModes, 0);

    SetChecked(IDC_MIC_ENABLE, true);
    SetChecked(IDC_SPK_ENABLE, false);

    HWND url = Item(IDC_OUTPUT_URL);
    for (const wchar_t* preset : kOutputPresets) {
        SendMessageW(url, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset));
    }
    SendMessageW(url, CB_SETCURSEL, 0, 0);

    FillCombo(Item(IDC_MUXER), kMuxers, 0);
    FillCombo(Item(IDC_RESOLUTION), kResolutions, kDefaultResolution);
    FillCombo(Item(IDC_FPS), kFps, kDefaultFps);
    FillCombo(Item(IDC_VCODEC), kVideoCodecs, 0);
    m_videoCodecAvailable.assign(std::size(kVideoCodecs), true);
    FillCombo(Item(IDC_ACODEC), kAudioCodecs, 0);
    FillCombo(Item(IDC_ABITRATE), kAudioBitrates, kDefaultAudioBitrate);
    FillCombo(Item(IDC_ROTATION), kRotations, 0);
    FillCombo(Item(IDC_FLIP), kFlips, 0);
    SetDlgItemTextW(m_hwnd, IDC_VBITRATE, L"3000");
    SetDlgItemTextW(m_hwnd, IDC_GOP, L"60");
    SetDlgItemTextW(m_hwnd, IDC_A_TIMEOUT, L"3000");
    SetDlgItemTextW(m_hwnd, IDC_V_TIMEOUT, L"3000");

    FillCombo(Item(IDC_LOGO_INDEX), kLogoIndexes, 0);
    FillCombo(Item(IDC_LOGO_POS), kLogoPositions, kDefaultLogoPos);
    SetDlgItemTextW(m_hwnd, IDC_LOGO_X, L"20");
    SetDlgItemTextW(m_hwnd, IDC_LOGO_Y, L"20");

    FillCombo(Item(IDC_LOG_LEVEL), kLogLevels, kDefaultLogLevel);
}

void MainDialog::FillCaptureModes()
{
    if (IsChecked(IDC_SRC_WINDOW)) {
        FillCombo(Item(IDC_CAP_MODE), kWindowModes, 0);
    } else {
        FillCombo(Item(IDC_CAP_MODE), kMonitorModes, 0);
    }
}

void MainDialog::RefreshMonitors()
{
    const int prev = Sel(IDC_MONITOR_LIST);
    m_monitors = EnumerateMonitors();
    HWND combo = Item(IDC_MONITOR_LIST);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& m : m_monitors) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m.label.c_str()));
    }
    SetSel(IDC_MONITOR_LIST, (prev >= 0 && prev < static_cast<int>(m_monitors.size())) ? prev : 0);
}

void MainDialog::RefreshWindows()
{
    const int prevIndex = Sel(IDC_WINDOW_LIST);
    const HWND prev = (prevIndex >= 0 && prevIndex < static_cast<int>(m_windows.size())) ? m_windows[prevIndex].hwnd : nullptr;
    m_windows = EnumerateWindows(m_hwnd);
    HWND combo = Item(IDC_WINDOW_LIST);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < m_windows.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m_windows[i].label.c_str()));
        if (m_windows[i].hwnd == prev) {
            sel = static_cast<int>(i);
        }
    }
    SetSel(IDC_WINDOW_LIST, sel);
    Log(Format(L"窗口列表已刷新：%zu 个可采集窗口", m_windows.size()));
}

void MainDialog::RefreshAudioDevices()
{
    auto refill = [this](int comboId, std::vector<AudioDevice>& list, bool capture) {
        const int prevIndex = Sel(comboId);
        const std::wstring prevId =
            (prevIndex > 0 && prevIndex - 1 < static_cast<int>(list.size())) ? list[prevIndex - 1].id : L"";
        list = EnumerateAudioDevices(capture);
        HWND combo = Item(comboId);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"系统默认设备（跟随系统设置）"));
        int sel = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            const std::wstring label = list[i].name + (list[i].isDefault ? L"（当前默认）" : L"");
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (!prevId.empty() && list[i].id == prevId) {
                sel = static_cast<int>(i) + 1;
            }
        }
        SetSel(comboId, sel);
    };
    refill(IDC_MIC_DEVICE, m_mics, true);
    refill(IDC_SPK_DEVICE, m_speakers, false);
}

void MainDialog::MarkUnavailableEncoders(const std::string& video, const std::string& audio)
{
    Log(L"本机可用编码器  视频：" + Utf8ToWide(video.empty() ? "（无）" : video) +
        L"   音频：" + Utf8ToWide(audio.empty() ? "（无）" : audio));
    if (video.empty()) {
        return;   // 探测失败时不标记，避免把全部选项误标为不可用
    }
    HWND combo = Item(IDC_VCODEC);
    const int sel = Sel(IDC_VCODEC);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < std::size(kVideoCodecs); ++i) {
        m_videoCodecAvailable[i] = ListContains(video, kVideoCodecs[i].value);
        const std::wstring label = std::wstring(kVideoCodecs[i].label) + (m_videoCodecAvailable[i] ? L"" : L"  ✗本机不可用");
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SetSel(IDC_VCODEC, sel);
}

// ------------------------------------------------------------------ 命令
void MainDialog::OnCommand(int id, int code)
{
    switch (id) {
        case IDC_SRC_MONITOR:
        case IDC_SRC_WINDOW:
            if (code == BN_CLICKED) {
                FillCaptureModes();
                if (IsChecked(IDC_SRC_WINDOW) && m_windows.empty()) {
                    RefreshWindows();
                }
                ApplyUiState();
            }
            break;
        case IDC_WINDOW_REFRESH:
            RefreshWindows();
            break;
        case IDC_AUDIO_REFRESH:
            RefreshAudioDevices();
            Log(Format(L"音频设备已刷新：麦克风 %zu 个，扬声器 %zu 个", m_mics.size(), m_speakers.size()));
            break;
        case IDC_MIC_ENABLE:
        case IDC_SPK_ENABLE:
            ApplyUiState();
            break;
        case IDC_MIC_MUTE:
            if (m_state == SessionState::Live) {
                m_session->MuteAudio(TypeOfSelfMicphone, IsChecked(IDC_MIC_MUTE));
            }
            break;
        case IDC_SPK_MUTE:
            if (m_state == SessionState::Live) {
                m_session->MuteAudio(TypeOfSpeaker, IsChecked(IDC_SPK_MUTE));
            }
            break;
        case IDC_OUTPUT_BROWSE:
            BrowseOutput();
            break;
        case IDC_PAUSE_PIC_BROWSE:
            BrowseImage(IDC_PAUSE_PIC);
            break;
        case IDC_LOGO_BROWSE:
            BrowseImage(IDC_LOGO_PATH);
            break;
        case IDC_PAUSE_PIC_APPLY:
            OnApplyPausePicture();
            break;
        case IDC_LOGO_POS:
            if (code == CBN_SELCHANGE) {
                ApplyUiState();
            }
            break;
        case IDC_LOGO_ADD:
            OnWatermark(static_cast<int>(LogoOpType::LOGO_ADD));
            break;
        case IDC_LOGO_UPDATE:
            OnWatermark(static_cast<int>(LogoOpType::LOGO_UPDATE));
            break;
        case IDC_LOGO_REMOVE:
            OnWatermark(static_cast<int>(LogoOpType::LOGO_RM));
            break;
        case IDC_START:
            OnStartStop();
            break;
        case IDC_PAUSE_VIDEO:
            if (m_state == SessionState::Live) {
                m_videoPaused = !m_videoPaused;
                m_session->MuteVideo(m_videoPaused);
                ApplyUiState();
            }
            break;
        case IDC_OPEN_OUTPUT_DIR:
            OpenOutputDir();
            break;
        case IDC_LOG_LEVEL:
            if (code == CBN_SELCHANGE) {
                const auto& level = Pick(kLogLevels, Sel(IDC_LOG_LEVEL));
                core::Logger::Instance().SetLevel(static_cast<core::LogLevel>(level.value));
                Log(std::wstring(L"日志级别切换为 ") + level.label);
            }
            break;
        case IDC_LOG_CLEAR:
            SetDlgItemTextW(m_hwnd, IDC_LOG, L"");
            break;
        case IDC_OPEN_LOG_DIR: {
            std::error_code ec;
            fs::create_directories(m_logDir, ec);
            ShellExecuteW(m_hwnd, L"open", m_logDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        default:
            // IDOK（回车）/ IDCANCEL（Esc）不关闭窗口，避免直播中误触退出；关闭请点右上角 ×
            break;
    }
}

void MainDialog::OnStartStop()
{
    if (m_state == SessionState::Idle) {
        LiveConfig cfg;
        if (!CollectConfig(cfg)) {
            return;
        }
        SaveSettings();
        m_videoPaused = false;
        m_lastStats.clear();
        m_state = SessionState::Connecting;   // 先本地切状态，防止连点
        ApplyUiState();
        m_session->Start(std::move(cfg));
    } else if (m_state == SessionState::Live) {
        m_state = SessionState::Stopping;
        ApplyUiState();
        m_session->Stop();
    }
}

bool MainDialog::CollectConfig(LiveConfig& cfg)
{
    auto fail = [this](const std::wstring& msg, int focusId) {
        MessageBoxW(m_hwnd, msg.c_str(), L"无法开始直播", MB_OK | MB_ICONWARNING);
        SetFocus(Item(focusId));
        return false;
    };

    // ---- 视频源 ----
    cfg.captureWindow = IsChecked(IDC_SRC_WINDOW);
    if (cfg.captureWindow) {
        const int i = Sel(IDC_WINDOW_LIST);
        if (i < 0 || i >= static_cast<int>(m_windows.size())) {
            return fail(L"请选择要采集的窗口（列表为空时点「刷新」）。", IDC_WINDOW_LIST);
        }
        cfg.window = m_windows[i].hwnd;
        if (!IsWindow(cfg.window)) {
            return fail(L"所选窗口已经关闭，请点「刷新」后重新选择。", IDC_WINDOW_REFRESH);
        }
        cfg.capMode = static_cast<VideoCapMode>(Pick(kWindowModes, Sel(IDC_CAP_MODE)).value);
    } else {
        if (m_monitors.empty()) {
            return fail(L"没有检测到显示器。", IDC_MONITOR_LIST);
        }
        cfg.monitorIndex = (std::max)(0, Sel(IDC_MONITOR_LIST));
        cfg.capMode = static_cast<VideoCapMode>(Pick(kMonitorModes, Sel(IDC_CAP_MODE)).value);
    }
    cfg.perfMode = Pick(kPerfModes, Sel(IDC_PERF_MODE)).value ? PerformanceMode::Gpu : PerformanceMode::Cpu;

    // ---- 音频源（下拉框第 0 项 = 系统默认设备）----
    auto deviceId = [this](int comboId, const std::vector<AudioDevice>& list) {
        const int i = Sel(comboId);
        return (i > 0 && i - 1 < static_cast<int>(list.size())) ? list[i - 1].id : std::wstring();
    };
    cfg.micEnabled = IsChecked(IDC_MIC_ENABLE);
    cfg.micDeviceId = deviceId(IDC_MIC_DEVICE, m_mics);
    cfg.micMuted = IsChecked(IDC_MIC_MUTE);
    cfg.speakerEnabled = IsChecked(IDC_SPK_ENABLE);
    cfg.speakerDeviceId = deviceId(IDC_SPK_DEVICE, m_speakers);
    cfg.speakerMuted = IsChecked(IDC_SPK_MUTE);

    // ---- 编码 ----
    const auto& res = Pick(kResolutions, Sel(IDC_RESOLUTION));
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = Pick(kFps, Sel(IDC_FPS)).value;
    cfg.videoCodec = Pick(kVideoCodecs, Sel(IDC_VCODEC)).value;
    cfg.audioCodec = Pick(kAudioCodecs, Sel(IDC_ACODEC)).value;
    cfg.audioBitrateKbps = Pick(kAudioBitrates, Sel(IDC_ABITRATE)).value;
    cfg.rotation = Pick(kRotations, Sel(IDC_ROTATION)).value;
    const int flip = Pick(kFlips, Sel(IDC_FLIP)).value;
    cfg.flipH = (flip & 1) != 0;
    cfg.flipV = (flip & 2) != 0;

    cfg.videoBitrateKbps = IntOf(IDC_VBITRATE, 0);
    if (cfg.videoBitrateKbps < 100 || cfg.videoBitrateKbps > 50000) {
        return fail(L"视频码率请填写 100 ~ 50000 之间的整数（单位 kbps）。", IDC_VBITRATE);
    }
    cfg.gop = IntOf(IDC_GOP, 0);
    if (cfg.gop < 1 || cfg.gop > 1000) {
        return fail(L"GOP 请填写 1 ~ 1000 之间的整数（单位：帧；常用值 = 帧率 × 2）。", IDC_GOP);
    }
    cfg.audioTimeoutMs = IntOf(IDC_A_TIMEOUT, 0);
    cfg.videoTimeoutMs = IntOf(IDC_V_TIMEOUT, 0);
    if (cfg.audioTimeoutMs < 100 || cfg.audioTimeoutMs > 60000) {
        return fail(L"音频断流告警阈值请填写 100 ~ 60000 毫秒。", IDC_A_TIMEOUT);
    }
    if (cfg.videoTimeoutMs < 100 || cfg.videoTimeoutMs > 60000) {
        return fail(L"视频断流告警阈值请填写 100 ~ 60000 毫秒。", IDC_V_TIMEOUT);
    }

    // ---- 输出 ----
    const std::wstring raw = Trim(Text(IDC_OUTPUT_URL));
    if (raw.empty()) {
        return fail(L"请填写输出地址：本地文件路径（.flv / .ts），或 rtmp:// / srt:// 推流地址。", IDC_OUTPUT_URL);
    }
    const std::wstring output = ExpandOutput(raw, m_exeDir);
    if (!IsNetworkUrl(output)) {
        std::error_code ec;
        fs::create_directories(fs::path(output).parent_path(), ec);
        if (ec) {
            return fail(L"无法创建输出目录：\n" + fs::path(output).parent_path().wstring(), IDC_OUTPUT_URL);
        }
    }
    const char* muxer = Pick(kMuxers, Sel(IDC_MUXER)).value;
    cfg.muxer = (*muxer != '\0') ? muxer : GuessMuxer(output);
    cfg.output = WideToUtf8(output);
    m_lastOutput = output;

    // ---- 暂停图片（可选）----
    const std::wstring pausePic = Trim(Text(IDC_PAUSE_PIC));
    if (!pausePic.empty()) {
        const std::wstring resolved = ResolvePath(pausePic);
        if (!IsNetworkUrl(resolved) && !fs::exists(resolved)) {
            return fail(L"暂停图片不存在：\n" + resolved + L"\n\n不需要时请清空该项（暂停时会冻结最后一帧）。", IDC_PAUSE_PIC);
        }
        cfg.pausePicture = WideToUtf8(resolved);
    }

    // ---- 组合提示 ----
    const int vcodec = Sel(IDC_VCODEC);
    if (vcodec >= 0 && vcodec < static_cast<int>(m_videoCodecAvailable.size()) && !m_videoCodecAvailable[vcodec]) {
        Log(L"提示：所选视频编码器本机不可用，内核会自动回落到 libopenh264（日志会出现 EncoderFallback）");
    }
    if (cfg.perfMode == PerformanceMode::Gpu) {
        if (cfg.videoCodec == "libopenh264") {
            Log(L"提示：GPU 模式需要硬件编码器，当前选的是软编，内核会按 CPU 链路运行");
        }
        if (cfg.captureWindow) {
            Log(L"提示：窗口采集不支持 GPU 直连，内核会自动回落 CPU 链路");
        }
        Log(L"提示：GPU 模式按采集分辨率编码，「分辨率」选项不生效");
    }
    if (!cfg.micEnabled && !cfg.speakerEnabled) {
        Log(L"提示：未开启任何音频源，本次为纯视频推流");
    }
    return true;
}

void MainDialog::OnApplyPausePicture()
{
    const std::wstring text = Trim(Text(IDC_PAUSE_PIC));
    const std::wstring resolved = text.empty() ? std::wstring() : ResolvePath(text);
    if (!resolved.empty() && !IsNetworkUrl(resolved) && !fs::exists(resolved)) {
        MessageBoxW(m_hwnd, (L"图片不存在：\n" + resolved).c_str(), L"暂停图片", MB_OK | MB_ICONWARNING);
        return;
    }
    m_session->SetPausePicture(WideToUtf8(resolved));
}

void MainDialog::OnWatermark(int op)
{
    WatermarkParams params;
    params.index = static_cast<uint32_t>(Pick(kLogoIndexes, Sel(IDC_LOGO_INDEX)).value);
    params.type = static_cast<LogoOpType>(op);
    params.position = Pick(kLogoPositions, Sel(IDC_LOGO_POS)).value;
    params.offset_x = IntOf(IDC_LOGO_X, 0);
    params.offset_y = IntOf(IDC_LOGO_Y, 0);
    if (params.type != LogoOpType::LOGO_RM) {
        const std::wstring text = Trim(Text(IDC_LOGO_PATH));
        if (text.empty()) {
            MessageBoxW(m_hwnd, L"请先选择 Logo 图片（PNG 带透明通道效果最好）。", L"Logo", MB_OK | MB_ICONWARNING);
            SetFocus(Item(IDC_LOGO_PATH));
            return;
        }
        const std::wstring resolved = ResolvePath(text);
        if (!IsNetworkUrl(resolved) && !fs::exists(resolved)) {
            MessageBoxW(m_hwnd, (L"图片不存在：\n" + resolved).c_str(), L"Logo", MB_OK | MB_ICONWARNING);
            return;
        }
        params.url = WideToUtf8(resolved);
    }
    m_session->SetupWatermark(std::move(params));
}

void MainDialog::BrowseOutput()
{
    const std::wstring dir = (fs::path(m_exeDir) / L"records").wstring();
    std::error_code ec;
    fs::create_directories(dir, ec);

    wchar_t file[MAX_PATH] = L"tg_record.flv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"FLV 文件 (*.flv)\0*.flv\0MPEG-TS 文件 (*.ts)\0*.ts\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"flv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) {
        SetDlgItemTextW(m_hwnd, IDC_OUTPUT_URL, file);
    }
}

void MainDialog::BrowseImage(int editId)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"图片 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetDlgItemTextW(m_hwnd, editId, file);
    }
}

void MainDialog::OpenOutputDir()
{
    // 优先打开最近一次实际录制的文件；没有录过就按当前填写的地址算目录
    std::wstring path = m_lastOutput;
    if (path.empty() || IsNetworkUrl(path)) {
        path = ExpandOutput(Trim(Text(IDC_OUTPUT_URL)), m_exeDir);
    }
    if (path.empty() || IsNetworkUrl(path)) {
        MessageBoxW(m_hwnd, L"当前输出是网络推流地址，没有本地目录。", L"打开输出目录", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const fs::path file(path);
    std::error_code ec;
    if (fs::exists(file, ec)) {
        const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(m_hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        fs::create_directories(file.parent_path(), ec);
        ShellExecuteW(m_hwnd, L"open", file.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ------------------------------------------------------------------ 会话事件（界面线程）
void MainDialog::OnSessionEvent(std::unique_ptr<SessionEvent> ev)
{
    switch (ev->type) {
        case SessionEvent::Type::StateChanged:
            m_state = ev->state;
            if (m_state == SessionState::Live) {
                m_liveSinceMs = GetTickCount64();
                m_videoPaused = false;
                SetTimer(m_hwnd, kTimerStats, 1000, nullptr);
                SetDlgItemTextW(m_hwnd, IDC_STATS, L"等待第一次统计…");
            } else if (m_state == SessionState::Idle) {
                KillTimer(m_hwnd, kTimerStats);
                m_videoPaused = false;
                if (!m_lastStats.empty()) {
                    SetDlgItemTextW(m_hwnd, IDC_STATS, (L"上一场直播的最后统计：\r\n" + m_lastStats).c_str());
                }
            }
            ApplyUiState();
            break;

        case SessionEvent::Type::Log:
            Log(ev->text);
            break;

        case SessionEvent::Type::Error:
            Log(Format(L"[回调] OnError  what=%d（%ls）  extra=%d  %ls", ev->code, ErrorName(ev->code), ev->extra,
                       ev->text.c_str()));
            if (IsFatalRuntimeError(ev->code) && m_state == SessionState::Live) {
                Log(L"运行期致命错误：同一连接无法恢复，自动停播。如需继续请重新点「开始直播」");
                m_state = SessionState::Stopping;
                ApplyUiState();
                m_session->Stop();
                MessageBoxW(m_hwnd,
                            Format(L"直播中断：%ls\n\n错误码 %d，底层码 %d\n%ls\n\n已自动停播，可检查网络 / 推流地址后重新开始。",
                                   ErrorName(ev->code), ev->code, ev->extra, ev->text.c_str())
                                .c_str(),
                            L"直播中断", MB_OK | MB_ICONERROR);
            }
            break;

        case SessionEvent::Type::StartFailed:
            Log(Format(L"开播失败：%ls  code=%d（%ls）", ev->text.c_str(), ev->code, ErrorName(ev->code)));
            MessageBoxW(m_hwnd,
                        Format(L"开播失败：%ls\n\n错误码 %d：%ls\n\n具体原因请看右侧日志（或 logs 目录下的日志文件）。",
                               ev->text.c_str(), ev->code, ErrorName(ev->code))
                            .c_str(),
                        L"开播失败", MB_OK | MB_ICONERROR);
            break;

        case SessionEvent::Type::Stats:
            m_lastStats = ev->text;
            UpdateStatsText();
            break;

        case SessionEvent::Type::EncodersProbed:
            MarkUnavailableEncoders(ev->videoEncoders, ev->audioEncoders);
            break;
    }
}

void MainDialog::OnTimer(UINT_PTR id)
{
    if (id == kTimerLog) {
        FlushLog();
    } else if (id == kTimerStats && m_state == SessionState::Live) {
        m_session->RequestStats();
    }
}

void MainDialog::UpdateStatsText()
{
    const ULONGLONG secs = (GetTickCount64() - m_liveSinceMs) / 1000;
    const std::wstring head = Format(L"直播时长 %02llu:%02llu:%02llu%ls\r\n", secs / 3600, secs / 60 % 60, secs % 60,
                                     m_videoPaused ? L"    （画面已暂停）" : L"");
    SetDlgItemTextW(m_hwnd, IDC_STATS, (head + m_lastStats).c_str());
}

// ------------------------------------------------------------------ 界面状态
void MainDialog::ApplyUiState()
{
    const bool idle = (m_state == SessionState::Idle);
    const bool live = (m_state == SessionState::Live);
    const bool runtime = idle || live;   // 开播前可预设、直播中可实时改的项
    const bool window = IsChecked(IDC_SRC_WINDOW);

    // ① 视频源：显示器 / 窗口两个下拉框同位置，按单选切换显示
    ShowWindow(Item(IDC_MONITOR_LIST), window ? SW_HIDE : SW_SHOW);
    ShowWindow(Item(IDC_WINDOW_LIST), window ? SW_SHOW : SW_HIDE);
    ShowWindow(Item(IDC_WINDOW_REFRESH), window ? SW_SHOW : SW_HIDE);
    SetDlgItemTextW(m_hwnd, IDC_TARGET_LABEL, window ? L"窗口" : L"显示器");
    Enable({IDC_SRC_MONITOR, IDC_SRC_WINDOW, IDC_MONITOR_LIST, IDC_WINDOW_LIST, IDC_WINDOW_REFRESH, IDC_CAP_MODE,
            IDC_PERF_MODE},
           idle);

    // ② 音频源：静音开播前可预设，直播中实时生效
    const bool mic = IsChecked(IDC_MIC_ENABLE);
    const bool spk = IsChecked(IDC_SPK_ENABLE);
    Enable({IDC_MIC_ENABLE, IDC_SPK_ENABLE, IDC_AUDIO_REFRESH}, idle);
    Enable(IDC_MIC_DEVICE, idle && mic);
    Enable(IDC_SPK_DEVICE, idle && spk);
    Enable(IDC_MIC_MUTE, runtime && mic);
    Enable(IDC_SPK_MUTE, runtime && spk);

    // ③ 编码与输出：只能开播前改
    Enable({IDC_OUTPUT_URL, IDC_OUTPUT_BROWSE, IDC_MUXER, IDC_RESOLUTION, IDC_FPS, IDC_VCODEC, IDC_VBITRATE, IDC_GOP,
            IDC_ROTATION, IDC_FLIP, IDC_A_TIMEOUT, IDC_V_TIMEOUT},
           idle);
    Enable({IDC_ACODEC, IDC_ABITRATE}, idle && (mic || spk));

    // ④ 画面控制
    Enable({IDC_PAUSE_PIC, IDC_PAUSE_PIC_BROWSE, IDC_LOGO_PATH, IDC_LOGO_BROWSE, IDC_LOGO_INDEX, IDC_LOGO_POS}, runtime);
    Enable(IDC_PAUSE_PIC_APPLY, live);
    const bool customPos = Pick(kLogoPositions, Sel(IDC_LOGO_POS)).value == -1;
    Enable({IDC_LOGO_X, IDC_LOGO_Y}, runtime && customPos);
    Enable({IDC_LOGO_ADD, IDC_LOGO_UPDATE, IDC_LOGO_REMOVE}, live);

    // 底部按钮
    const wchar_t* startText = L"开始直播";
    switch (m_state) {
        case SessionState::Connecting: startText = L"正在连接…"; break;
        case SessionState::Live: startText = L"停止直播"; break;
        case SessionState::Stopping: startText = L"正在停止…"; break;
        default: break;
    }
    SetDlgItemTextW(m_hwnd, IDC_START, startText);
    Enable(IDC_START, idle || live);
    Enable(IDC_PAUSE_VIDEO, live);
    SetDlgItemTextW(m_hwnd, IDC_PAUSE_VIDEO, m_videoPaused ? L"恢复画面" : L"暂停画面");

    // 状态文字
    std::wstring status = StateLabel(m_state);
    if (live) {
        status += m_videoPaused ? L"（画面已暂停）" : L"";
        status += L"   → " + m_lastOutput;
    }
    m_statusColor = StateColor(m_state);
    SetDlgItemTextW(m_hwnd, IDC_STATUS, status.c_str());
    InvalidateRect(Item(IDC_STATUS), nullptr, TRUE);
}

void MainDialog::OnSize(int width, int height)
{
    if (m_anchorRects.size() != std::size(kAnchors)) {
        return;   // 尚未完成初始化
    }
    const int dx = (std::max)(0, width - static_cast<int>(m_baseClient.cx));
    const int dy = (std::max)(0, height - static_cast<int>(m_baseClient.cy));
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(std::size(kAnchors)));
    for (size_t i = 0; i < std::size(kAnchors); ++i) {
        const auto& a = kAnchors[i];
        const RECT& r = m_anchorRects[i];
        const int x = r.left + (a.moveX ? dx : 0);
        const int w = (r.right - r.left) + (a.growX ? dx : 0);
        const int h = (r.bottom - r.top) + (a.growY ? dy : 0);
        dwp = DeferWindowPos(dwp, Item(a.id), nullptr, x, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(dwp);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void MainDialog::OnClose()
{
    if (m_state != SessionState::Idle) {
        const int answer = MessageBoxW(m_hwnd, L"正在直播（或正在连接）。\n确定停止并退出吗？", L"退出",
                                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) {
            return;
        }
    }
    SaveSettings();
    KillTimer(m_hwnd, kTimerLog);
    KillTimer(m_hwnd, kTimerStats);
    m_statusColor = StateColor(SessionState::Stopping);
    SetDlgItemTextW(m_hwnd, IDC_STATUS, L"● 正在停止并退出…");
    UpdateWindow(m_hwnd);

    // 同步停播：Stop 会排空编码器并写文件尾，录制文件才完整
    m_session->Shutdown();
    // 工作线程已退出；丢弃已投递但还没处理的事件，释放其内存
    MSG msg;
    while (PeekMessageW(&msg, m_hwnd, WM_SESSION_EVENT, WM_SESSION_EVENT, PM_REMOVE)) {
        delete reinterpret_cast<SessionEvent*>(msg.lParam);
    }
    EndDialog(m_hwnd, 0);
}

// ------------------------------------------------------------------ 日志
void MainDialog::Log(const std::wstring& text)
{
    LogBuffer::Instance().Push(L'U', text);
    FlushLog();
}

void MainDialog::FlushLog()
{
    const std::wstring text = LogBuffer::Instance().Drain();
    if (text.empty()) {
        return;
    }
    HWND edit = Item(IDC_LOG);
    // 界面上最多保留约 40 万字符，超出时砍掉最早的一半（完整日志在文件里）
    constexpr int kMaxChars = 400000;
    int len = GetWindowTextLengthW(edit);
    if (len + static_cast<int>(text.size()) > kMaxChars) {
        SendMessageW(edit, EM_SETSEL, 0, kMaxChars / 2);
        SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        len = GetWindowTextLengthW(edit);
    }
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

// ------------------------------------------------------------------ 设置持久化
void MainDialog::LoadSettings()
{
    const wchar_t* ini = m_iniPath.c_str();
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        return;   // 首次运行：全部用默认值
    }

    // 单选决定"采集方式"下拉框的内容，必须先恢复
    const bool window = GetPrivateProfileIntW(kIniSection, L"source_window", 0, ini) != 0;
    SetChecked(IDC_SRC_MONITOR, !window);
    SetChecked(IDC_SRC_WINDOW, window);
    FillCaptureModes();
    if (window) {
        RefreshWindows();
    }

    for (const auto& item : kPersist) {
        switch (item.kind) {
            case Kind::Combo: {
                const int index = static_cast<int>(GetPrivateProfileIntW(kIniSection, item.key, -1, ini));
                const int count = static_cast<int>(SendMessageW(Item(item.id), CB_GETCOUNT, 0, 0));
                if (index >= 0 && index < count) {
                    SetSel(item.id, index);
                }
                break;
            }
            case Kind::Check: {
                const int v = static_cast<int>(GetPrivateProfileIntW(kIniSection, item.key, -1, ini));
                if (v >= 0) {
                    SetChecked(item.id, v != 0);
                }
                break;
            }
            case Kind::Text: {
                wchar_t buf[2048] = {};
                const DWORD n = GetPrivateProfileStringW(kIniSection, item.key, L"\x1", buf, 2048, ini);
                if (!(n == 1 && buf[0] == L'\x1')) {
                    SetDlgItemTextW(m_hwnd, item.id, buf);
                }
                break;
            }
        }
    }

    // 音频设备按端点 id 匹配（设备插拔后下标会变）
    auto restoreDevice = [&](const wchar_t* key, int comboId, const std::vector<AudioDevice>& list) {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(kIniSection, key, L"", buf, 512, ini);
        for (size_t i = 0; buf[0] != L'\0' && i < list.size(); ++i) {
            if (list[i].id == buf) {
                SetSel(comboId, static_cast<int>(i) + 1);
            }
        }
    };
    restoreDevice(L"mic_device", IDC_MIC_DEVICE, m_mics);
    restoreDevice(L"spk_device", IDC_SPK_DEVICE, m_speakers);
}

void MainDialog::SaveSettings()
{
    const wchar_t* ini = m_iniPath.c_str();
    // 先写入 UTF-16 BOM，后续 WritePrivateProfileStringW 才会按 Unicode 保存（中文路径不乱码）
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE file = CreateFileW(ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const WORD bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(file, &bom, sizeof(bom), &written, nullptr);
            CloseHandle(file);
        }
    }
    auto put = [ini](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(kIniSection, key, value.c_str(), ini);
    };

    put(L"source_window", IsChecked(IDC_SRC_WINDOW) ? L"1" : L"0");
    for (const auto& item : kPersist) {
        switch (item.kind) {
            case Kind::Combo: put(item.key, std::to_wstring(Sel(item.id))); break;
            case Kind::Check: put(item.key, IsChecked(item.id) ? L"1" : L"0"); break;
            case Kind::Text: put(item.key, Text(item.id)); break;
        }
    }
    auto deviceId = [this](int comboId, const std::vector<AudioDevice>& list) {
        const int i = Sel(comboId);
        return (i > 0 && i - 1 < static_cast<int>(list.size())) ? list[i - 1].id : std::wstring();
    };
    put(L"mic_device", deviceId(IDC_MIC_DEVICE, m_mics));
    put(L"spk_device", deviceId(IDC_SPK_DEVICE, m_speakers));
}

// ------------------------------------------------------------------ 控件小工具
int MainDialog::Sel(int id) const
{
    return static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0));
}

void MainDialog::SetSel(int id, int index)
{
    SendMessageW(Item(id), CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

std::wstring MainDialog::Text(int id) const
{
    HWND ctl = Item(id);
    const int len = GetWindowTextLengthW(ctl);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(ctl, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

int MainDialog::IntOf(int id, int fallback) const
{
    const std::wstring s = Trim(Text(id));
    return s.empty() ? fallback : _wtoi(s.c_str());
}

void MainDialog::Enable(std::initializer_list<int> ids, bool on)
{
    for (int id : ids) {
        Enable(id, on);
    }
}

std::wstring MainDialog::ResolvePath(const std::wstring& text) const
{
    if (IsNetworkUrl(text)) {
        return text;
    }
    fs::path p(text);
    if (p.is_relative()) {
        p = fs::path(m_exeDir) / p;
    }
    return p.lexically_normal().wstring();
}

}  // namespace tgui
