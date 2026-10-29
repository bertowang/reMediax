#include "MainDialog.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
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

constexpr IntChoice kMonitorModes[] = {{L"DXGI 桌面复制（推荐）", DXGI}, {L"GDI（兼容兜底）", GDI}};
constexpr IntChoice kWindowModes[] = {{L"整窗（含标题栏）", WGC}, {L"仅客户区（去标题栏）", WGC_No_TBar}};
constexpr IntChoice kPerfModes[] = {{L"CPU（兼容性最好）", 0}, {L"GPU（显存直送硬编）", 1}};
constexpr TextChoice kMuxers[] = {{L"自动（按地址判断）", ""}, {L"flv", "flv"}, {L"mpegts", "mpegts"}};
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
constexpr IntChoice kOverlayKinds[] = {{L"图片（png / jpg）", static_cast<int>(OverlayKind::Image)},
                                       {L"视频文件（画中画）", static_cast<int>(OverlayKind::VideoFile)}};
constexpr IntChoice kLogLevels[] = {{L"Debug", 0}, {L"Info", 1}, {L"Warn", 2}, {L"Error", 3}};

constexpr const wchar_t* kImageFilter = L"图片 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件 (*.*)\0*.*\0";
constexpr const wchar_t* kVideoFilter = L"视频 (*.mp4;*.mov;*.flv;*.mkv)\0*.mp4;*.mov;*.flv;*.mkv\0所有文件 (*.*)\0*.*\0";
constexpr const wchar_t* kAudioFilter =
    L"带 AAC 音轨的文件 (*.m4a;*.aac;*.mp4;*.mov;*.flv)\0*.m4a;*.aac;*.mp4;*.mov;*.flv\0所有文件 (*.*)\0*.*\0";

// 输出地址预设：{time} 在开播时替换为当前时间，相对路径以 exe 目录为基准
constexpr const wchar_t* kOutputPresets[] = {
    L"records\\tg_{time}.flv",
    L"records\\tg_{time}.ts",
    L"rtmp://127.0.0.1/live/stream",
    L"srt://127.0.0.1:10080?streamid=#!::r=live/stream,m=publish",
};

// 默认值
constexpr const char* kDefaultPreset = "720p";
constexpr int kDefaultFps = 2;          // 30fps
constexpr int kDefaultAudioBitrate = 2; // 128kbps
constexpr int kDefaultLogoPos = 3;      // 右下角
constexpr int kDefaultLogLevel = 1;     // Info
constexpr int kVolumeMax = 200;         // 音量滑块 0 ~ 200%（SDK 允许 0 ~ 400%）
constexpr int kLevelRangeDb = 60;       // 电平条：-60 dBFS ~ 0 dBFS

constexpr UINT_PTR kTimerLog = 1;      // 200ms：把缓冲的日志刷到界面
constexpr UINT_PTR kTimerStats = 2;    // 1s：直播中拉一次统计
constexpr UINT_PTR kTimerLevels = 3;   // 150ms：直播中拉一次电平

// 三路音频源对应的控件
struct SlotControls
{
    AudioSlot slot;
    int enable;
    int device;   // Mic / Speaker：设备下拉框；File：路径输入框
    int mute;     // File：循环勾选
    int volume;
    int level;
};
constexpr SlotControls kSlots[] = {
    {AudioSlot::Mic, IDC_MIC_ENABLE, IDC_MIC_DEVICE, IDC_MIC_MUTE, IDC_MIC_VOLUME, IDC_MIC_LEVEL},
    {AudioSlot::Speaker, IDC_SPK_ENABLE, IDC_SPK_DEVICE, IDC_SPK_MUTE, IDC_SPK_VOLUME, IDC_SPK_LEVEL},
    {AudioSlot::File, IDC_FILE_ENABLE, IDC_FILE_PATH, IDC_FILE_LOOP, IDC_FILE_VOLUME, IDC_FILE_LEVEL},
};
const SlotControls& SlotOf(AudioSlot slot)
{
    return kSlots[static_cast<int>(slot)];
}

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
    Text,
    Slider
};
struct PersistItem
{
    const wchar_t* key;
    int id;
    Kind kind;
};
constexpr PersistItem kPersist[] = {
    {L"cap_mode", IDC_CAP_MODE, Kind::Combo},       {L"perf_mode", IDC_PERF_MODE, Kind::Combo},
    {L"monitor", IDC_MONITOR_LIST, Kind::Combo},    {L"vfile_path", IDC_VFILE_PATH, Kind::Text},
    {L"vfile_loop", IDC_VFILE_LOOP, Kind::Check},   {L"vfile_audio", IDC_VFILE_AUDIO, Kind::Check},
    {L"mic_enable", IDC_MIC_ENABLE, Kind::Check},   {L"mic_mute", IDC_MIC_MUTE, Kind::Check},
    {L"mic_volume", IDC_MIC_VOLUME, Kind::Slider},  {L"spk_enable", IDC_SPK_ENABLE, Kind::Check},
    {L"spk_mute", IDC_SPK_MUTE, Kind::Check},       {L"spk_volume", IDC_SPK_VOLUME, Kind::Slider},
    {L"file_enable", IDC_FILE_ENABLE, Kind::Check}, {L"file_path", IDC_FILE_PATH, Kind::Text},
    {L"file_loop", IDC_FILE_LOOP, Kind::Check},     {L"file_volume", IDC_FILE_VOLUME, Kind::Slider},
    {L"output", IDC_OUTPUT_URL, Kind::Text},        {L"muxer", IDC_MUXER, Kind::Combo},
    {L"res_preset", IDC_RESOLUTION, Kind::Combo},   {L"fps", IDC_FPS, Kind::Combo},
    {L"vcodec", IDC_VCODEC, Kind::Combo},           {L"vbitrate", IDC_VBITRATE, Kind::Text},
    {L"gop", IDC_GOP, Kind::Text},                  {L"acodec", IDC_ACODEC, Kind::Combo},
    {L"abitrate", IDC_ABITRATE, Kind::Combo},       {L"rotation", IDC_ROTATION, Kind::Combo},
    {L"flip", IDC_FLIP, Kind::Combo},               {L"audio_timeout", IDC_A_TIMEOUT, Kind::Text},
    {L"video_timeout", IDC_V_TIMEOUT, Kind::Text},  {L"pause_picture", IDC_PAUSE_PIC, Kind::Text},
    {L"logo_path", IDC_LOGO_PATH, Kind::Text},      {L"logo_index", IDC_LOGO_INDEX, Kind::Combo},
    {L"logo_pos", IDC_LOGO_POS, Kind::Combo},       {L"logo_x", IDC_LOGO_X, Kind::Text},
    {L"logo_y", IDC_LOGO_Y, Kind::Text},            {L"ovl_id", IDC_OVL_ID, Kind::Text},
    {L"ovl_kind", IDC_OVL_KIND, Kind::Combo},       {L"ovl_path", IDC_OVL_PATH, Kind::Text},
    {L"ovl_x", IDC_OVL_X, Kind::Text},              {L"ovl_y", IDC_OVL_Y, Kind::Text},
    {L"ovl_w", IDC_OVL_W, Kind::Text},              {L"ovl_h", IDC_OVL_H, Kind::Text},
    {L"ovl_opacity", IDC_OVL_OPACITY, Kind::Text},  {L"ovl_z", IDC_OVL_Z, Kind::Text},
    {L"ovl_visible", IDC_OVL_VISIBLE, Kind::Check}, {L"ovl_loop", IDC_OVL_LOOP, Kind::Check},
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

std::vector<std::string> SplitCsv(const std::string& csv)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        std::string item = csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty()) {
            out.push_back(std::move(item));
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return out;
}

bool ListContains(const std::string& csv, const char* name)
{
    const auto items = SplitCsv(csv);
    return std::find(items.begin(), items.end(), name) != items.end();
}

// 叠加层 id：字母数字 . _ -（与 SDK 的校验一致，界面上先拦一道，提示更直接）
bool IsValidOverlayId(const std::wstring& id)
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' ||
               c == L'_' || c == L'-';
    });
}

bool SameOverlay(const OverlayDesc& a, const OverlayDesc& b)
{
    return a.id == b.id && a.kind == b.kind && a.path == b.path && a.loop == b.loop && a.x == b.x && a.y == b.y &&
           a.width == b.width && a.height == b.height && a.opacity == b.opacity && a.zOrder == b.zOrder &&
           a.visible == b.visible;
}

const OverlayDesc* FindOverlay(const std::vector<OverlayDesc>& list, const std::string& id)
{
    for (const auto& d : list) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

int Percent(float v)
{
    return static_cast<int>(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
}

const wchar_t* StateLabel(SessionState s)
{
    switch (s) {
        case SessionState::Idle: return L"● 未开播";
        case SessionState::Previewing: return L"● 预览中（未推流）";
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
        case SessionState::Previewing: return RGB(0, 110, 200);
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
        case WM_HSCROLL:
            if (lp != 0) {
                OnVolumeScroll(reinterpret_cast<HWND>(lp));
            }
            return TRUE;
        case WM_NOTIFY:
            OnNotify(*reinterpret_cast<const NMHDR*>(lp));
            return FALSE;   // 不拦截任何通知的默认处理
        case WM_DRAWITEM:
            if (wp == IDC_PREVIEW_VIEW) {
                m_preview.Paint(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
                return TRUE;
            }
            return FALSE;
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

    m_preview.Attach(Item(IDC_PREVIEW_VIEW));
    InitOverlayList();
    FillStaticOptions();
    RefreshMonitors();
    RefreshAudioDevices();
    LoadSettings();

    // 预览帧回调直接交给 PreviewView（在内核预览线程上转换，再通知界面重绘）
    m_session = std::make_unique<LiveSession>(m_hwnd, [this](const PreviewFrame& frame) { m_preview.OnFrame(frame); });
    SetTimer(m_hwnd, kTimerLog, 200, nullptr);

    Log(L"界面就绪。可以直接「开始直播」，也可以先「开启预览」摆好叠加层再开播；日志同时写入 " + m_logDir);
    Log(L"正在后台探测本机可用的编码器…");
    m_session->ProbeEncoders();

    SetDlgItemTextW(m_hwnd, IDC_STATS, L"开播后每秒刷新：分辨率、码率、已写出数据量、编码帧数、丢帧、积压。");
    ApplyUiState();
}

void MainDialog::FillStaticOptions()
{
    SetChecked(IDC_SRC_MONITOR, true);
    FillCaptureModes();
    FillCombo(Item(IDC_PERF_MODE), kPerfModes, 0);
    SetChecked(IDC_VFILE_LOOP, true);
    SetChecked(IDC_VFILE_AUDIO, false);

    SetChecked(IDC_MIC_ENABLE, true);
    SetChecked(IDC_SPK_ENABLE, false);
    SetChecked(IDC_FILE_ENABLE, false);
    SetChecked(IDC_FILE_LOOP, true);
    for (const auto& s : kSlots) {
        HWND slider = Item(s.volume);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kVolumeMax));
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(slider, TBM_SETPOS, TRUE, 100);
        SendMessageW(Item(s.level), PBM_SETRANGE32, 0, kLevelRangeDb);
    }
    SendMessageW(Item(IDC_MASTER_LEVEL), PBM_SETRANGE32, 0, kLevelRangeDb);

    HWND url = Item(IDC_OUTPUT_URL);
    for (const wchar_t* preset : kOutputPresets) {
        SendMessageW(url, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset));
    }
    SendMessageW(url, CB_SETCURSEL, 0, 0);

    FillCombo(Item(IDC_MUXER), kMuxers, 0);
    FillResolutionPresets();
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

    // 叠加层表单默认值：右上角、宽 20%、按素材比例
    SetDlgItemTextW(m_hwnd, IDC_OVL_ID, L"logo");
    FillCombo(Item(IDC_OVL_KIND), kOverlayKinds, 0);
    SetDlgItemTextW(m_hwnd, IDC_OVL_X, L"75");
    SetDlgItemTextW(m_hwnd, IDC_OVL_Y, L"5");
    SetDlgItemTextW(m_hwnd, IDC_OVL_W, L"20");
    SetDlgItemTextW(m_hwnd, IDC_OVL_H, L"0");
    SetDlgItemTextW(m_hwnd, IDC_OVL_OPACITY, L"100");
    SetDlgItemTextW(m_hwnd, IDC_OVL_Z, L"10");
    SetChecked(IDC_OVL_VISIBLE, true);
    SetChecked(IDC_OVL_LOOP, true);

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

void MainDialog::FillResolutionPresets()
{
    // 档位表以 SDK 为准（ITGCapturerMgr::GetResolutionPresets），尺寸 / 帧率 / 建议码率用 ApplyResolutionPreset 取
    m_presets.clear();
    for (const auto& name : SplitCsv(ITGCapturerMgr::GetResolutionPresets())) {
        VideoEncoderParams p;
        if (!ITGCapturerMgr::ApplyResolutionPreset(name, p)) {
            continue;
        }
        ResolutionPreset preset;
        preset.name = name;
        preset.width = p.width;
        preset.height = p.height;
        preset.fps = static_cast<int>(p.fps + 0.5f);
        preset.bitrateKbps = p.vBitrate / 1000;
        preset.label = Format(L"%ls  %dx%d  %dfps  %dk", Utf8ToWide(name).c_str(), p.width, p.height, preset.fps,
                              preset.bitrateKbps);
        m_presets.push_back(std::move(preset));
    }
    HWND combo = Item(IDC_RESOLUTION);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < m_presets.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m_presets[i].label.c_str()));
        if (m_presets[i].name == kDefaultPreset) {
            sel = static_cast<int>(i);
        }
    }
    SetSel(IDC_RESOLUTION, sel);
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
        case IDC_SRC_FILE:
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
        case IDC_VFILE_BROWSE:
            BrowseFile(IDC_VFILE_PATH, kVideoFilter);
            break;
        case IDC_VFILE_AUDIO:
            ApplyUiState();
            break;
        case IDC_AUDIO_REFRESH:
            RefreshAudioDevices();
            Log(Format(L"音频设备已刷新：麦克风 %zu 个，扬声器 %zu 个", m_mics.size(), m_speakers.size()));
            break;
        case IDC_MIC_ENABLE:
            OnAudioSlotToggled(AudioSlot::Mic);
            break;
        case IDC_SPK_ENABLE:
            OnAudioSlotToggled(AudioSlot::Speaker);
            break;
        case IDC_FILE_ENABLE:
            OnAudioSlotToggled(AudioSlot::File);
            break;
        case IDC_MIC_MUTE:
            if (HasCapturer()) {
                m_session->MuteAudio(AudioSlot::Mic, IsChecked(IDC_MIC_MUTE));
            }
            break;
        case IDC_SPK_MUTE:
            if (HasCapturer()) {
                m_session->MuteAudio(AudioSlot::Speaker, IsChecked(IDC_SPK_MUTE));
            }
            break;
        case IDC_FILE_BROWSE:
            BrowseFile(IDC_FILE_PATH, kAudioFilter);
            break;
        case IDC_OUTPUT_BROWSE:
            BrowseOutput();
            break;
        case IDC_RESOLUTION:
            if (code == CBN_SELCHANGE) {
                OnResolutionSelected();
            }
            break;
        case IDC_RES_APPLY:
            OnSwitchResolution();
            break;
        case IDC_PAUSE_PIC_BROWSE:
            BrowseFile(IDC_PAUSE_PIC, kImageFilter);
            break;
        case IDC_LOGO_BROWSE:
            BrowseFile(IDC_LOGO_PATH, kImageFilter);
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
        case IDC_OVL_KIND:
            if (code == CBN_SELCHANGE) {
                ApplyUiState();
            }
            break;
        case IDC_OVL_BROWSE: {
            const bool video = Pick(kOverlayKinds, Sel(IDC_OVL_KIND)).value == static_cast<int>(OverlayKind::VideoFile);
            BrowseFile(IDC_OVL_PATH, video ? kVideoFilter : kImageFilter);
            break;
        }
        case IDC_OVL_STAGE:
            OnStageOverlay();
            break;
        case IDC_OVL_REMOVE:
            OnRemoveOverlay();
            break;
        case IDC_OVL_COMMIT:
            m_session->CommitOverlays();
            break;
        case IDC_OVL_DISCARD:
            m_session->DiscardOverlays();
            break;
        case IDC_START:
            OnStartStop();
            break;
        case IDC_PREVIEW:
            OnPreviewToggle();
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
    if (m_pending) {
        return;
    }
    if (m_state == SessionState::Idle || m_state == SessionState::Previewing) {
        LiveConfig cfg;
        if (!CollectConfig(cfg, /*forStart*/ true)) {
            return;
        }
        SaveSettings();
        m_videoPaused = false;
        m_lastStats.clear();
        m_liveHasAudio = cfg.HasAudio();
        m_state = SessionState::Connecting;   // 先本地切状态，防止连点
        m_pending = true;
        ApplyUiState();
        m_session->Start(std::move(cfg));
    } else if (m_state == SessionState::Live) {
        m_state = SessionState::Stopping;
        m_pending = true;
        ApplyUiState();
        m_session->Stop();
    }
}

void MainDialog::OnPreviewToggle()
{
    if (m_pending) {
        return;
    }
    if (m_previewOn) {
        m_pending = true;
        ApplyUiState();
        m_session->StopPreview();
        return;
    }
    if (m_state != SessionState::Idle && m_state != SessionState::Live) {
        return;
    }
    // 直播中开预览：实例已在，cfg 不会被用到；未开播时按界面配置创建实例
    LiveConfig cfg;
    if (m_state == SessionState::Idle) {
        if (!CollectConfig(cfg, /*forStart*/ false)) {
            return;
        }
        SaveSettings();
    }
    m_pending = true;
    m_preview.Reset(L"正在打开采集…");
    ApplyUiState();
    m_session->StartPreview(std::move(cfg));
}

bool MainDialog::CollectAudioSlot(AudioSlot slot, AudioSlotConfig& out, bool quiet)
{
    const SlotControls& c = SlotOf(slot);
    out.enabled = IsChecked(c.enable);
    out.volume = static_cast<float>(SliderPos(c.volume)) / 100.0f;
    if (slot == AudioSlot::File) {
        out.loop = IsChecked(c.mute);
        const std::wstring text = Trim(Text(c.device));
        const std::wstring resolved = text.empty() ? std::wstring() : ResolvePath(text);
        if (out.enabled && (resolved.empty() || !fs::exists(resolved))) {
            if (!quiet) {
                MessageBoxW(m_hwnd,
                            (text.empty() ? std::wstring(L"请先选择文件音频（带 AAC 音轨的 m4a / mp4 / flv）。")
                                          : L"文件不存在：\n" + resolved)
                                .c_str(),
                            L"文件音频", MB_OK | MB_ICONWARNING);
                SetFocus(Item(c.device));
            }
            return false;
        }
        out.filePath = WideToUtf8(resolved);
        return true;
    }
    const std::vector<AudioDevice>& list = (slot == AudioSlot::Mic) ? m_mics : m_speakers;
    const int i = Sel(c.device);   // 第 0 项 = 系统默认设备
    out.deviceId = (i > 0 && i - 1 < static_cast<int>(list.size())) ? list[i - 1].id : std::wstring();
    out.muted = IsChecked(c.mute);
    return true;
}

void MainDialog::OnAudioSlotToggled(AudioSlot slot)
{
    const SlotControls& c = SlotOf(slot);
    if (HasCapturer() && !m_pending) {
        // 预览 / 直播中：勾选 = AddAudioSource，取消 = RemoveAudioSource（推流不中断，淡入淡出 20ms）
        AudioSlotConfig cfg;
        if (!CollectAudioSlot(slot, cfg, /*quiet*/ false)) {
            SetChecked(c.enable, false);
        } else {
            m_session->SetAudioSlot(slot, std::move(cfg));
        }
    }
    ApplyUiState();
}

bool MainDialog::CollectConfig(LiveConfig& cfg, bool forStart)
{
    const wchar_t* title = forStart ? L"无法开始直播" : L"无法开启预览";
    auto fail = [this, title](const std::wstring& msg, int focusId) {
        MessageBoxW(m_hwnd, msg.c_str(), title, MB_OK | MB_ICONWARNING);
        SetFocus(Item(focusId));
        return false;
    };
    const bool fresh = !HasCapturer();   // 已有实例（预览中）时视频源 / 音频源都在实例上，不再校验

    // ---- 视频源 ----
    cfg.source = SourceKind();
    switch (cfg.source) {
        case VideoSourceKind::Window: {
            cfg.capMode = static_cast<VideoCapMode>(Pick(kWindowModes, Sel(IDC_CAP_MODE)).value);
            const int i = Sel(IDC_WINDOW_LIST);
            if (fresh && (i < 0 || i >= static_cast<int>(m_windows.size()))) {
                return fail(L"请选择要采集的窗口（列表为空时点「刷新」）。", IDC_WINDOW_LIST);
            }
            cfg.window = (i >= 0 && i < static_cast<int>(m_windows.size())) ? m_windows[i].hwnd : nullptr;
            if (fresh && !IsWindow(cfg.window)) {
                return fail(L"所选窗口已经关闭，请点「刷新」后重新选择。", IDC_WINDOW_REFRESH);
            }
            break;
        }
        case VideoSourceKind::Monitor:
            if (fresh && m_monitors.empty()) {
                return fail(L"没有检测到显示器。", IDC_MONITOR_LIST);
            }
            cfg.monitorIndex = (std::max)(0, Sel(IDC_MONITOR_LIST));
            cfg.capMode = static_cast<VideoCapMode>(Pick(kMonitorModes, Sel(IDC_CAP_MODE)).value);
            break;
        case VideoSourceKind::File: {
            cfg.capMode = LocalVideoFile;
            const std::wstring text = Trim(Text(IDC_VFILE_PATH));
            const std::wstring resolved = text.empty() ? std::wstring() : ResolvePath(text);
            if (fresh && text.empty()) {
                return fail(L"请选择本地视频文件（mp4 / mov / flv，H.264 视频轨）。", IDC_VFILE_PATH);
            }
            if (fresh && !fs::exists(resolved)) {
                return fail(L"视频文件不存在：\n" + resolved, IDC_VFILE_PATH);
            }
            cfg.videoFile = WideToUtf8(resolved);
            cfg.videoFileLoop = IsChecked(IDC_VFILE_LOOP);
            cfg.videoFileAudio = IsChecked(IDC_VFILE_AUDIO);
            break;
        }
    }
    cfg.perfMode = Pick(kPerfModes, Sel(IDC_PERF_MODE)).value ? PerformanceMode::Gpu : PerformanceMode::Cpu;

    // ---- 音频源 ----
    for (const auto& s : kSlots) {
        if (!CollectAudioSlot(s.slot, cfg.Slot(s.slot), /*quiet*/ !fresh)) {
            if (fresh) {
                return false;   // 已弹框提示
            }
            cfg.Slot(s.slot).enabled = false;
        }
    }

    // ---- 编码 ----
    const int presetIndex = Sel(IDC_RESOLUTION);
    if (presetIndex >= 0 && presetIndex < static_cast<int>(m_presets.size())) {
        const auto& p = m_presets[presetIndex];
        cfg.resolutionPreset = p.name;
        cfg.width = p.width;
        cfg.height = p.height;
    }
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

    // ---- 输出（只有开播需要）----
    if (forStart) {
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
    }

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
    if (forStart && vcodec >= 0 && vcodec < static_cast<int>(m_videoCodecAvailable.size()) &&
        !m_videoCodecAvailable[vcodec]) {
        Log(L"提示：所选视频编码器本机不可用，内核会自动回落到 libopenh264（日志会出现 EncoderFallback）");
    }
    if (fresh && cfg.perfMode == PerformanceMode::Gpu) {
        if (cfg.videoCodec == "libopenh264") {
            Log(L"提示：GPU 模式需要硬件编码器，当前选的是软编，内核会按 CPU 链路运行");
        }
        if (cfg.source != VideoSourceKind::Monitor) {
            Log(L"提示：只有显示器采集支持 GPU 直连，窗口 / 文件源会自动回落 CPU 链路");
        }
        Log(L"提示：GPU 直连时按采集分辨率编码（「分辨率」不生效），叠加层也只画在 CPU 帧上");
    }
    if (forStart && !cfg.HasAudio()) {
        Log(L"提示：未开启任何音频源，本次为纯视频推流（直播中也不能再加音频源）");
    }
    return true;
}

void MainDialog::OnResolutionSelected()
{
    const int i = Sel(IDC_RESOLUTION);
    if (i < 0 || i >= static_cast<int>(m_presets.size())) {
        return;
    }
    const auto& p = m_presets[i];
    // 档位自带建议码率 / 帧率：开播前一并填上（仍可手改）；帧率在实例创建后就不能再改
    if (m_state == SessionState::Idle) {
        for (size_t k = 0; k < std::size(kFps); ++k) {
            if (kFps[k].value == p.fps) {
                SetSel(IDC_FPS, static_cast<int>(k));
            }
        }
    }
    if (m_state == SessionState::Idle || m_state == SessionState::Previewing) {
        SetDlgItemTextW(m_hwnd, IDC_VBITRATE, std::to_wstring(p.bitrateKbps).c_str());
    }
    if (HasCapturer()) {
        Log(L"已选档位 " + Utf8ToWide(p.name) + L"，点「切换」生效");
    }
}

void MainDialog::OnSwitchResolution()
{
    const int i = Sel(IDC_RESOLUTION);
    if (!HasCapturer() || i < 0 || i >= static_cast<int>(m_presets.size())) {
        return;
    }
    m_session->SwitchResolution(m_presets[i].name);
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

void MainDialog::BrowseFile(int editId, const wchar_t* filter)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = filter;
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

// ------------------------------------------------------------------ 叠加层
void MainDialog::InitOverlayList()
{
    HWND list = Item(IDC_OVL_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    struct Column
    {
        const wchar_t* title;
        int widthDlu;
    };
    constexpr Column kColumns[] = {{L"ID", 38}, {L"类型", 22}, {L"X,Y  宽×高 %", 60}, {L"不透明", 26}, {L"层", 14}, {L"状态", 36}};
    for (int i = 0; i < static_cast<int>(std::size(kColumns)); ++i) {
        RECT r{0, 0, kColumns[i].widthDlu, 0};
        MapDialogRect(m_hwnd, &r);   // 对话框单位 → 像素（跟随字体 / DPI）
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(kColumns[i].title);
        col.cx = r.right;
        ListView_InsertColumn(list, i, &col);
    }
}

bool MainDialog::CollectOverlay(OverlayDesc& desc)
{
    auto fail = [this](const std::wstring& msg, int focusId) {
        MessageBoxW(m_hwnd, msg.c_str(), L"叠加层", MB_OK | MB_ICONWARNING);
        SetFocus(Item(focusId));
        return false;
    };
    const std::wstring id = Trim(Text(IDC_OVL_ID));
    if (!IsValidOverlayId(id)) {
        return fail(L"ID 必填，只能用字母、数字和 . _ -（同 ID 再次暂存 = 整体替换该层）。", IDC_OVL_ID);
    }
    desc.id = WideToUtf8(id);
    desc.kind = static_cast<OverlayKind>(Pick(kOverlayKinds, Sel(IDC_OVL_KIND)).value);
    const std::wstring path = Trim(Text(IDC_OVL_PATH));
    if (path.empty()) {
        return fail(desc.kind == OverlayKind::Image ? L"请选择图片（png 带透明通道效果最好）。"
                                                    : L"请选择视频文件（mp4 / mov / flv，H.264 视频轨）。",
                    IDC_OVL_PATH);
    }
    const std::wstring resolved = ResolvePath(path);
    if (!fs::exists(resolved)) {
        return fail(L"文件不存在：\n" + resolved, IDC_OVL_PATH);
    }
    desc.path = WideToUtf8(resolved);

    int x = 0, y = 0, w = 0, h = 0, opacity = 0, z = 0;
    if (!ReadInt(IDC_OVL_X, x) || x < -100 || x > 100) {
        return fail(L"X% 请填 -100 ~ 100（左上角相对画布宽的比例，允许部分移出画面）。", IDC_OVL_X);
    }
    if (!ReadInt(IDC_OVL_Y, y) || y < -100 || y > 100) {
        return fail(L"Y% 请填 -100 ~ 100（左上角相对画布高的比例）。", IDC_OVL_Y);
    }
    if (!ReadInt(IDC_OVL_W, w) || w < 1 || w > 200) {
        return fail(L"宽% 请填 1 ~ 200（相对画布宽）。", IDC_OVL_W);
    }
    if (!ReadInt(IDC_OVL_H, h) || h < 0 || h > 200) {
        return fail(L"高% 请填 0 ~ 200（相对画布高；0 = 按素材宽高比）。", IDC_OVL_H);
    }
    if (!ReadInt(IDC_OVL_OPACITY, opacity) || opacity < 0 || opacity > 100) {
        return fail(L"不透明度% 请填 0 ~ 100。", IDC_OVL_OPACITY);
    }
    if (!ReadInt(IDC_OVL_Z, z)) {
        return fail(L"层级请填整数（大的画在上面）。", IDC_OVL_Z);
    }
    desc.x = static_cast<float>(x) / 100.0f;
    desc.y = static_cast<float>(y) / 100.0f;
    desc.width = static_cast<float>(w) / 100.0f;
    desc.height = static_cast<float>(h) / 100.0f;
    desc.opacity = static_cast<float>(opacity) / 100.0f;
    desc.zOrder = z;
    desc.visible = IsChecked(IDC_OVL_VISIBLE);
    desc.loop = IsChecked(IDC_OVL_LOOP);
    return true;
}

void MainDialog::OnStageOverlay()
{
    OverlayDesc desc;
    if (CollectOverlay(desc)) {
        m_session->StageOverlay(std::move(desc));
    }
}

void MainDialog::OnRemoveOverlay()
{
    // 以表单里的 ID 为准；表单为空时用列表选中行
    std::string id = WideToUtf8(Trim(Text(IDC_OVL_ID)));
    if (id.empty()) {
        const int sel = ListView_GetNextItem(Item(IDC_OVL_LIST), -1, LVNI_SELECTED);
        if (sel >= 0 && sel < static_cast<int>(m_ovlRows.size())) {
            id = m_ovlRows[sel];
        }
    }
    if (id.empty()) {
        MessageBoxW(m_hwnd, L"请填写要移除的叠加层 ID，或在列表里选中一行。", L"叠加层", MB_OK | MB_ICONWARNING);
        return;
    }
    m_session->RemoveOverlay(std::move(id));
}

void MainDialog::LoadOverlayForm(const OverlayDesc& d)
{
    SetDlgItemTextW(m_hwnd, IDC_OVL_ID, Utf8ToWide(d.id).c_str());
    SetSel(IDC_OVL_KIND, d.kind == OverlayKind::VideoFile ? 1 : 0);
    SetDlgItemTextW(m_hwnd, IDC_OVL_PATH, Utf8ToWide(d.path).c_str());
    SetDlgItemTextW(m_hwnd, IDC_OVL_X, std::to_wstring(Percent(d.x)).c_str());
    SetDlgItemTextW(m_hwnd, IDC_OVL_Y, std::to_wstring(Percent(d.y)).c_str());
    SetDlgItemTextW(m_hwnd, IDC_OVL_W, std::to_wstring(Percent(d.width)).c_str());
    SetDlgItemTextW(m_hwnd, IDC_OVL_H, std::to_wstring(Percent(d.height)).c_str());
    SetDlgItemTextW(m_hwnd, IDC_OVL_OPACITY, std::to_wstring(Percent(d.opacity)).c_str());
    SetDlgItemTextW(m_hwnd, IDC_OVL_Z, std::to_wstring(d.zOrder).c_str());
    SetChecked(IDC_OVL_VISIBLE, d.visible);
    SetChecked(IDC_OVL_LOOP, d.loop);
    ApplyUiState();
}

void MainDialog::RefreshOverlayList()
{
    HWND list = Item(IDC_OVL_LIST);
    m_fillingOverlayList = true;   // 重建期间的选中变化不回填表单（避免覆盖正在编辑的内容）

    std::string selectedId;
    const int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < static_cast<int>(m_ovlRows.size())) {
        selectedId = m_ovlRows[sel];
    }
    ListView_DeleteAllItems(list);
    m_ovlRows.clear();

    auto addRow = [&](const OverlayDesc& d, std::wstring status) {
        if (!d.visible) {
            status += L" · 隐藏";
        }
        std::wstring cells[] = {
            Utf8ToWide(d.id),
            d.kind == OverlayKind::VideoFile ? L"视频" : L"图片",
            Format(L"%d,%d  %d×%ls", Percent(d.x), Percent(d.y), Percent(d.width),
                   d.height > 0 ? std::to_wstring(Percent(d.height)).c_str() : L"自动"),
            Format(L"%d%%", Percent(d.opacity)),
            std::to_wstring(d.zOrder),
            status,
        };
        const int row = static_cast<int>(m_ovlRows.size());
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = cells[0].data();
        ListView_InsertItem(list, &item);
        for (int col = 1; col < static_cast<int>(std::size(cells)); ++col) {
            ListView_SetItemText(list, row, col, cells[col].data());
        }
        m_ovlRows.push_back(d.id);
    };

    // 暂存场景（= 预览画面）按绘制顺序列出，标出与推流画面的差别；只在推流里、已被暂存移除的排在最后
    bool dirty = false;
    for (const auto& d : m_ovlStaged) {
        const OverlayDesc* c = FindOverlay(m_ovlCommitted, d.id);
        const bool same = c && SameOverlay(*c, d);
        dirty = dirty || !same;
        addRow(d, !c ? L"新增 · 未提交" : same ? L"已提交" : L"已改 · 未提交");
    }
    for (const auto& c : m_ovlCommitted) {
        if (!FindOverlay(m_ovlStaged, c.id)) {
            dirty = true;
            addRow(c, L"移除 · 未提交");
        }
    }
    for (size_t i = 0; i < m_ovlRows.size(); ++i) {
        if (!selectedId.empty() && m_ovlRows[i] == selectedId) {
            ListView_SetItemState(list, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    m_fillingOverlayList = false;

    SetDlgItemTextW(m_hwnd, IDC_OVL_STATUS,
                    Format(L"推流 %zu 层 · 预览 %zu 层%ls", m_ovlCommitted.size(), m_ovlStaged.size(),
                           dirty ? L" · 有未提交改动" : L"")
                        .c_str());
}

void MainDialog::OnNotify(const NMHDR& hdr)
{
    if (hdr.idFrom != IDC_OVL_LIST || hdr.code != LVN_ITEMCHANGED || m_fillingOverlayList) {
        return;
    }
    const auto& nm = reinterpret_cast<const NMLISTVIEW&>(hdr);
    if (!(nm.uChanged & LVIF_STATE) || !(nm.uNewState & LVIS_SELECTED) || (nm.uOldState & LVIS_SELECTED)) {
        return;
    }
    if (nm.iItem < 0 || nm.iItem >= static_cast<int>(m_ovlRows.size())) {
        return;
    }
    const std::string& id = m_ovlRows[nm.iItem];
    const OverlayDesc* d = FindOverlay(m_ovlStaged, id);
    if (!d) {
        d = FindOverlay(m_ovlCommitted, id);
    }
    if (d) {
        LoadOverlayForm(*d);
    }
}

// ------------------------------------------------------------------ 会话事件（界面线程）
void MainDialog::OnSessionEvent(std::unique_ptr<SessionEvent> ev)
{
    switch (ev->type) {
        case SessionEvent::Type::StateChanged:
            OnStateChanged(ev->state, ev->preview);
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
                m_pending = true;
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

        case SessionEvent::Type::PreviewFailed:
            m_pending = false;
            Log(Format(L"开启预览失败：%ls  code=%d（%ls）", ev->text.c_str(), ev->code, ErrorName(ev->code)));
            if (!m_previewOn) {
                m_preview.Reset(L"预览未开启");
            }
            ApplyUiState();
            MessageBoxW(m_hwnd,
                        Format(L"开启预览失败：%ls\n\n错误码 %d：%ls\n\n具体原因请看右侧日志。", ev->text.c_str(), ev->code,
                               ErrorName(ev->code))
                            .c_str(),
                        L"预览", MB_OK | MB_ICONERROR);
            break;

        case SessionEvent::Type::Stats:
            m_lastStats = ev->text;
            UpdateStatsText();
            break;

        case SessionEvent::Type::EncodersProbed:
            MarkUnavailableEncoders(ev->videoEncoders, ev->audioEncoders);
            break;

        case SessionEvent::Type::Levels:
            if (m_state == SessionState::Live) {
                for (const auto& s : kSlots) {
                    SetLevel(s.level, ev->levels[static_cast<int>(s.slot)]);
                }
                SetLevel(IDC_MASTER_LEVEL, ev->levels[kAudioSlotCount]);
            }
            break;

        case SessionEvent::Type::AudioSlotChanged:
            if (ev->code >= 0 && ev->code < kAudioSlotCount) {
                SetChecked(kSlots[ev->code].enable, ev->extra != 0);
                ApplyUiState();
            }
            break;

        case SessionEvent::Type::Overlays:
            m_ovlCommitted = std::move(ev->committed);
            m_ovlStaged = std::move(ev->staged);
            RefreshOverlayList();
            break;
    }
}

void MainDialog::OnStateChanged(SessionState state, bool preview)
{
    const SessionState prev = m_state;
    const bool prevPreview = m_previewOn;
    m_state = state;
    m_previewOn = preview;
    m_pending = false;

    if (state == SessionState::Live) {
        if (prev != SessionState::Live) {
            m_liveSinceMs = GetTickCount64();
            m_videoPaused = false;
            SetTimer(m_hwnd, kTimerStats, 1000, nullptr);
            SetTimer(m_hwnd, kTimerLevels, 150, nullptr);
            SetDlgItemTextW(m_hwnd, IDC_STATS, L"等待第一次统计…");
        }
    } else {
        KillTimer(m_hwnd, kTimerStats);
        KillTimer(m_hwnd, kTimerLevels);
        ResetLevels();
        m_videoPaused = false;
        if ((prev == SessionState::Live || prev == SessionState::Stopping) && !m_lastStats.empty()) {
            SetDlgItemTextW(m_hwnd, IDC_STATS, (L"上一场直播的最后统计：\r\n" + m_lastStats).c_str());
        }
    }

    if (preview && !prevPreview) {
        m_preview.Reset(L"预览已开启，等待第一帧…");
    } else if (!preview && (prevPreview || state == SessionState::Idle)) {
        m_preview.Reset(state == SessionState::Live ? L"预览已关闭（推流不受影响）" : L"预览未开启");
    }
    ApplyUiState();
}

void MainDialog::OnTimer(UINT_PTR id)
{
    if (id == kTimerLog) {
        FlushLog();
    } else if (id == kTimerStats && m_state == SessionState::Live) {
        m_session->RequestStats();
    } else if (id == kTimerLevels && m_state == SessionState::Live) {
        m_session->RequestLevels();
    }
}

void MainDialog::OnVolumeScroll(HWND trackbar)
{
    for (const auto& s : kSlots) {
        if (Item(s.volume) == trackbar) {
            if (HasCapturer()) {
                m_session->SetAudioVolume(s.slot, static_cast<float>(SliderPos(s.volume)) / 100.0f);
            }
            return;
        }
    }
}

void MainDialog::UpdateStatsText()
{
    const ULONGLONG secs = (GetTickCount64() - m_liveSinceMs) / 1000;
    const std::wstring head = Format(L"直播时长 %02llu:%02llu:%02llu%ls\r\n", secs / 3600, secs / 60 % 60, secs % 60,
                                     m_videoPaused ? L"    （画面已暂停）" : L"");
    SetDlgItemTextW(m_hwnd, IDC_STATS, (head + m_lastStats).c_str());
}

void MainDialog::SetLevel(int barId, float peakDb)
{
    const int pos = (std::clamp)(static_cast<int>(peakDb + kLevelRangeDb + 0.5f), 0, kLevelRangeDb);
    HWND bar = Item(barId);
    // 进度条（Common Controls v6）往上涨时有动画、会拖后半秒；先多走一格再退回，退回是立即生效的
    if (pos >= kLevelRangeDb) {
        SendMessageW(bar, PBM_SETRANGE32, 0, kLevelRangeDb + 1);
        SendMessageW(bar, PBM_SETPOS, kLevelRangeDb + 1, 0);
        SendMessageW(bar, PBM_SETPOS, kLevelRangeDb, 0);
        SendMessageW(bar, PBM_SETRANGE32, 0, kLevelRangeDb);
    } else {
        SendMessageW(bar, PBM_SETPOS, pos + 1, 0);
        SendMessageW(bar, PBM_SETPOS, pos, 0);
    }
}

void MainDialog::ResetLevels()
{
    for (const auto& s : kSlots) {
        SendMessageW(Item(s.level), PBM_SETPOS, 0, 0);
    }
    SendMessageW(Item(IDC_MASTER_LEVEL), PBM_SETPOS, 0, 0);
}

// ------------------------------------------------------------------ 界面状态
VideoSourceKind MainDialog::SourceKind() const
{
    if (IsChecked(IDC_SRC_FILE)) {
        return VideoSourceKind::File;
    }
    return IsChecked(IDC_SRC_WINDOW) ? VideoSourceKind::Window : VideoSourceKind::Monitor;
}

void MainDialog::ApplyUiState()
{
    const bool idle = (m_state == SessionState::Idle);
    const bool previewing = (m_state == SessionState::Previewing);
    const bool live = (m_state == SessionState::Live);
    const bool stable = (idle || previewing || live) && !m_pending;   // 没有在途的开播 / 停播 / 预览请求
    const bool capturer = HasCapturer();
    const VideoSourceKind source = SourceKind();

    // ① 视频源：三种对象共用同一行，按单选切换显示；实例存在期间不能更换
    Show({IDC_MONITOR_LIST}, source == VideoSourceKind::Monitor);
    Show({IDC_WINDOW_LIST, IDC_WINDOW_REFRESH}, source == VideoSourceKind::Window);
    Show({IDC_VFILE_PATH, IDC_VFILE_BROWSE, IDC_VFILE_LOOP, IDC_VFILE_AUDIO}, source == VideoSourceKind::File);
    Show({IDC_CAP_MODE_LABEL, IDC_CAP_MODE}, source != VideoSourceKind::File);
    SetDlgItemTextW(m_hwnd, IDC_TARGET_LABEL,
                    source == VideoSourceKind::Window ? L"窗口" : source == VideoSourceKind::File ? L"视频文件" : L"显示器");
    Enable({IDC_SRC_MONITOR, IDC_SRC_WINDOW, IDC_SRC_FILE, IDC_MONITOR_LIST, IDC_WINDOW_LIST, IDC_WINDOW_REFRESH,
            IDC_CAP_MODE, IDC_PERF_MODE, IDC_VFILE_PATH, IDC_VFILE_BROWSE, IDC_VFILE_LOOP, IDC_VFILE_AUDIO},
           idle && stable);

    // ② 音频源：开关在预览 / 直播中实时生效（纯视频推流时直播中不能再开）；
    //    设备 / 文件只能在该路关着时改（改完再勾上 = 按新设备接入）；音量随时可调
    const bool audioSwitchable = stable && (idle || previewing || (live && m_liveHasAudio));
    for (const auto& s : kSlots) {
        const bool on = IsChecked(s.enable);
        Enable(s.enable, audioSwitchable);
        Enable(s.device, stable && !(capturer && on));
        Enable(s.mute, stable && (s.slot == AudioSlot::File ? !(capturer && on) : on));
        Enable(s.volume, stable);
    }
    Enable(IDC_FILE_BROWSE, stable && !(capturer && IsChecked(IDC_FILE_ENABLE)));
    Enable(IDC_AUDIO_REFRESH, idle && stable);

    // ③ 编码与输出：码率 / 编码器 / 输出在开播前（含预览中）可改；帧率 / 旋转 / 翻转跟随实例，创建后不能改；
    //    分辨率档位任何时候都能切（预览 / 直播中点「切换」）
    const bool beforeLive = stable && (idle || previewing);
    Enable({IDC_OUTPUT_URL, IDC_OUTPUT_BROWSE, IDC_MUXER, IDC_VCODEC, IDC_VBITRATE, IDC_GOP, IDC_A_TIMEOUT,
            IDC_V_TIMEOUT},
           beforeLive);
    Enable({IDC_FPS, IDC_ROTATION, IDC_FLIP}, idle && stable);
    bool anyAudio = (source == VideoSourceKind::File && IsChecked(IDC_VFILE_AUDIO));
    for (const auto& s : kSlots) {
        anyAudio = anyAudio || IsChecked(s.enable);
    }
    Enable({IDC_ACODEC, IDC_ABITRATE}, beforeLive && anyAudio);
    Enable(IDC_RESOLUTION, stable);
    Enable(IDC_RES_APPLY, stable && capturer);

    // ④ 画面控制
    Enable({IDC_PAUSE_PIC, IDC_PAUSE_PIC_BROWSE, IDC_LOGO_PATH, IDC_LOGO_BROWSE, IDC_LOGO_INDEX, IDC_LOGO_POS}, stable);
    Enable(IDC_PAUSE_PIC_APPLY, stable && live);
    const bool customPos = Pick(kLogoPositions, Sel(IDC_LOGO_POS)).value == -1;
    Enable({IDC_LOGO_X, IDC_LOGO_Y}, stable && customPos);
    Enable({IDC_LOGO_ADD, IDC_LOGO_UPDATE, IDC_LOGO_REMOVE}, stable && live);

    // ⑤ 叠加层：表单随时可填，操作要有实例（预览或直播中）
    const bool video = Pick(kOverlayKinds, Sel(IDC_OVL_KIND)).value == static_cast<int>(OverlayKind::VideoFile);
    Enable(IDC_OVL_LOOP, video);
    Enable({IDC_OVL_STAGE, IDC_OVL_REMOVE, IDC_OVL_COMMIT, IDC_OVL_DISCARD, IDC_OVL_LIST}, stable && capturer);

    // 底部按钮
    const wchar_t* startText = L"开始直播";
    switch (m_state) {
        case SessionState::Connecting: startText = L"正在连接…"; break;
        case SessionState::Live: startText = L"停止直播"; break;
        case SessionState::Stopping: startText = L"正在停止…"; break;
        default: break;
    }
    SetDlgItemTextW(m_hwnd, IDC_START, startText);
    Enable(IDC_START, stable);
    SetDlgItemTextW(m_hwnd, IDC_PREVIEW, m_previewOn ? L"关闭预览" : L"开启预览");
    Enable(IDC_PREVIEW, stable);
    Enable(IDC_PAUSE_VIDEO, stable && live);
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
        const int answer = MessageBoxW(m_hwnd, L"正在直播 / 预览（或正在连接）。\n确定停止并退出吗？", L"退出",
                                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) {
            return;
        }
    }
    SaveSettings();
    KillTimer(m_hwnd, kTimerLog);
    KillTimer(m_hwnd, kTimerStats);
    KillTimer(m_hwnd, kTimerLevels);
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

    // 单选决定"采集方式"下拉框的内容，必须先恢复（source_kind：0 显示器 / 1 窗口 / 2 文件；兼容旧版的 source_window）
    const int legacyWindow = static_cast<int>(GetPrivateProfileIntW(kIniSection, L"source_window", 0, ini));
    const int kind = static_cast<int>(GetPrivateProfileIntW(kIniSection, L"source_kind", legacyWindow ? 1 : 0, ini));
    SetChecked(IDC_SRC_MONITOR, kind == 0);
    SetChecked(IDC_SRC_WINDOW, kind == 1);
    SetChecked(IDC_SRC_FILE, kind == 2);
    FillCaptureModes();
    if (kind == 1) {
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
            case Kind::Slider: {
                const int v = static_cast<int>(GetPrivateProfileIntW(kIniSection, item.key, -1, ini));
                if (v >= 0 && v <= kVolumeMax) {
                    SendMessageW(Item(item.id), TBM_SETPOS, TRUE, v);
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

    put(L"source_kind", std::to_wstring(static_cast<int>(SourceKind())));
    for (const auto& item : kPersist) {
        switch (item.kind) {
            case Kind::Combo: put(item.key, std::to_wstring(Sel(item.id))); break;
            case Kind::Check: put(item.key, IsChecked(item.id) ? L"1" : L"0"); break;
            case Kind::Slider: put(item.key, std::to_wstring(SliderPos(item.id))); break;
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

bool MainDialog::ReadInt(int id, int& out) const
{
    const std::wstring s = Trim(Text(id));
    if (s.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const long v = std::wcstol(s.c_str(), &end, 10);
    if (!end || *end != L'\0' || v < -1000000 || v > 1000000) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

int MainDialog::SliderPos(int id) const
{
    return static_cast<int>(SendMessageW(Item(id), TBM_GETPOS, 0, 0));
}

void MainDialog::Enable(std::initializer_list<int> ids, bool on)
{
    for (int id : ids) {
        Enable(id, on);
    }
}

void MainDialog::Show(std::initializer_list<int> ids, bool on)
{
    for (int id : ids) {
        ShowWindow(Item(id), on ? SW_SHOW : SW_HIDE);
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
