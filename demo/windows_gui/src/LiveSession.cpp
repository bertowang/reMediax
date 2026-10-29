#include "LiveSession.h"

#include <algorithm>
#include <cstdlib>
#include <map>

#include "StringUtil.h"
// 指定音频端点时直接用平台层的 WASAPI 源（与命令行 demo 相同做法）；
// 只用系统默认设备的宿主走公开 API AddAudioSource(AudioSourceDesc) 即可，无需包含它
#include "platform/windows/WinAudioCapture.h"

namespace tgui
{

using namespace TGCapture;

// ------------------------------------------------------------------ 错误码 / 通知说明
const wchar_t* ErrorName(int code)
{
    switch (code) {
        case TGErr_OK: return L"成功";
        case TGErr_InvalidState: return L"状态不允许该操作";
        case TGErr_NotConfigured: return L"编码 / 封装参数未设全";
        case TGErr_OpenVideoEncoder: return L"视频编码器打开失败";
        case TGErr_OpenAudioEncoder: return L"音频编码器打开失败";
        case TGErr_OpenMuxer: return L"输出地址打开失败（推流连不上 / 文件无法创建）";
        case TGErr_OpenVideoSource: return L"视频采集源打不开";
        case TGErr_StartFailed: return L"内部线程启动失败";
        case TGErr_InvalidArgument: return L"参数非法";
        case TGErr_NotSupported: return L"当前平台 / 模式不支持";
        case TGErr_NotImplemented: return L"接口尚未实现";
        case TGErr_Internal: return L"内部资源创建失败";
        case TGErr_MuxWriteFailed: return L"写出持续失败（断网 / 对端不收 / 磁盘满）";
        case TGErr_VideoEncodeFailed: return L"视频编码持续失败";
        case TGErr_AudioEncodeFailed: return L"音频编码持续失败";
        case TGErr_VideoCaptureStalled: return L"视频采集断流";
        case TGErr_AudioCaptureStalled: return L"音频采集断流";
        case TGErr_MuxTrailerFailed: return L"收尾写文件尾失败（文件可能缺索引）";
        default: return L"未知错误";
    }
}

bool IsFatalRuntimeError(int code)
{
    return code == TGErr_MuxWriteFailed || code == TGErr_VideoEncodeFailed || code == TGErr_AudioEncodeFailed;
}

namespace
{

const wchar_t* NotifyName(NotifyMsgType type)
{
    switch (type) {
        case EsCap_Notify_Fisrt_Frame_OK: return L"首帧就绪";
        case EsCap_Notify_VideoMetaInfo_Changed: return L"编码分辨率已切换";
        case EsCap_Notify_AudioMetaInfo_Changed: return L"音频参数变化";
        case EsCap_Notify_SystemFreeze: return L"检测到系统卡顿 / 挂起";
        case EsCap_Notify_LocalFile_EOF: return L"本地文件播放结束";
        case EsCap_Notify_VideoCapture_Recovered: return L"视频采集已恢复";
        case EsCap_Notify_AudioCapture_Recovered: return L"音频采集已恢复";
        case EsCap_Notify_Encoder_Fallback: return L"编码器回落";
        case EsCap_Notify_AudioSource_Added: return L"音频源已接入";
        case EsCap_Notify_AudioSource_Removed: return L"音频源已移除";
        case EsCap_Notify_Overlay_Committed: return L"叠加层已提交";
        default: return L"通知";
    }
}

const wchar_t* SlotName(AudioSlot slot)
{
    switch (slot) {
        case AudioSlot::Mic: return L"麦克风";
        case AudioSlot::Speaker: return L"系统声音";
        case AudioSlot::File: return L"文件音频";
    }
    return L"音频源";
}

// 界面上的固定 id：便于在日志 / 电平里对上号（AddAudioSource 不传 id 时内核也会生成 mic / speaker / file）
const char* SlotId(AudioSlot slot)
{
    switch (slot) {
        case AudioSlot::Mic: return "mic";
        case AudioSlot::Speaker: return "speaker";
        case AudioSlot::File: return "file";
    }
    return "";
}

std::wstring RetText(int ret)
{
    return ret == 0 ? std::wstring(L"成功") : Format(L"失败 ret=%d（%ls）", ret, ErrorName(ret));
}

// GetStatsInfo 返回的是扁平 JSON，key 都是已知 ASCII：取第一个匹配项即可，不引入 JSON 库
double JsonNumber(const std::string& json, const char* key)
{
    const std::string pattern = std::string("\"") + key + "\":";
    const size_t pos = json.find(pattern);
    return pos == std::string::npos ? 0.0 : std::strtod(json.c_str() + pos + pattern.size(), nullptr);
}

std::string JsonString(const std::string& json, const char* key)
{
    const std::string pattern = std::string("\"") + key + "\":\"";
    const size_t pos = json.find(pattern);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t begin = pos + pattern.size();
    const size_t end = json.find('"', begin);
    return end == std::string::npos ? std::string() : json.substr(begin, end - begin);
}

VideoEncoderParams BuildVideoParams(const LiveConfig& cfg)
{
    VideoEncoderParams vp;
    vp.ResolutionName = cfg.resolutionPreset;
    vp.width = cfg.width;
    vp.height = cfg.height;
    vp.fps = static_cast<float>(cfg.fps);
    vp.vBitrate = cfg.videoBitrateKbps * 1000;
    vp.gop = cfg.gop;
    vp.vCodec = cfg.videoCodec;
    vp.vEncoderType = (cfg.videoCodec == "h264_nvenc") ? h264_nvenc : libopenh264;
    vp.perfMode = cfg.perfMode;
    vp.videoRotation = cfg.rotation;
    vp.videoFlipHorizontal = cfg.flipH;
    vp.videoFlipVertical = cfg.flipV;
    return vp;
}

}  // namespace

// ------------------------------------------------------------------ 构造 / 任务队列
LiveSession::LiveSession(HWND notifyWnd, ITGCapturer::OnPreviewFrameCallback previewCallback)
    : m_notifyWnd(notifyWnd), m_previewCallback(std::move(previewCallback))
{
    m_worker = std::thread([this] { WorkerLoop(); });
}

LiveSession::~LiveSession()
{
    Shutdown();
}

void LiveSession::Shutdown()
{
    if (!m_worker.joinable()) {
        return;
    }
    Enqueue([this] { Teardown(); });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    m_worker.join();
}

void LiveSession::Enqueue(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_quit) {
            return;
        }
        m_tasks.push_back(std::move(task));
    }
    m_cv.notify_one();
}

void LiveSession::WorkerLoop()
{
    // WASAPI 枚举 / 打开需要 COM；采集源内部各自初始化，这里给工作线程本身也备一份
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_quit || !m_tasks.empty(); });
            if (m_tasks.empty()) {
                break;   // m_quit 且队列已排空：Shutdown 投递的 Teardown 一定已执行
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task();
    }
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
}

// ------------------------------------------------------------------ 对外接口（投递）
void LiveSession::ProbeEncoders()
{
    Enqueue([this] {
        // 硬编探测会真的创建一次编码会话（几十 ~ 几百 ms），所以放在工作线程
        auto ev = std::make_unique<SessionEvent>();
        ev->type = SessionEvent::Type::EncodersProbed;
        ev->videoEncoders = ITGCapturerMgr::GetAvailableEncoders(true);
        ev->audioEncoders = ITGCapturerMgr::GetAvailableEncoders(false);
        Emit(std::move(ev));
    });
}

void LiveSession::StartPreview(LiveConfig cfg)
{
    Enqueue([this, cfg = std::move(cfg)] { DoStartPreview(cfg); });
}

void LiveSession::StopPreview()
{
    Enqueue([this] { DoStopPreview(); });
}

void LiveSession::Start(LiveConfig cfg)
{
    Enqueue([this, cfg = std::move(cfg)] { DoStart(cfg); });
}

void LiveSession::Stop()
{
    Enqueue([this] { DoStop(); });
}

void LiveSession::MuteVideo(bool mute)
{
    Enqueue([this, mute] {
        if (m_capturer) {
            m_capturer->MuteVideo(mute);
            EmitLog(mute ? L"已暂停画面（MuteVideo=true）" : L"已恢复画面（MuteVideo=false）");
        }
    });
}

void LiveSession::MuteAudio(AudioSlot slot, bool mute)
{
    Enqueue([this, slot, mute] {
        SlotRuntime& rt = m_slots[static_cast<int>(slot)];
        rt.muted = mute;   // 之后重新接入（换设备）时沿用
        if (!m_capturer || rt.id.empty()) {
            return;
        }
        const AudioCapSourceType type = slot == AudioSlot::Speaker ? TypeOfSpeaker : TypeOfSelfMicphone;
        const bool applied = m_capturer->MuteAudio(mute, type);
        EmitLog(Format(L"%ls%ls%ls", SlotName(slot), mute ? L"已静音" : L"已取消静音", applied ? L"" : L"（该路未开启，未生效）"));
    });
}

void LiveSession::SetAudioSlot(AudioSlot slot, AudioSlotConfig cfg)
{
    Enqueue([this, slot, cfg = std::move(cfg)] {
        if (!m_capturer) {
            return;   // 还没有实例：配置在创建时按界面上的勾选一次性接入
        }
        RemoveSlot(slot);
        if (!cfg.enabled) {
            EmitSlot(slot, false);
            return;
        }
        if (m_state.load() == SessionState::Live && !m_liveHasAudio) {
            EmitLog(Format(L"本场是纯视频推流（开播时没有任何音频源），不能再加%ls；停播后勾选再开播", SlotName(slot)));
            EmitSlot(slot, false);
            return;
        }
        EmitSlot(slot, AddSlot(slot, cfg) == 0);
    });
}

void LiveSession::SetAudioVolume(AudioSlot slot, float volume)
{
    Enqueue([this, slot, volume] {
        const std::string& id = m_slots[static_cast<int>(slot)].id;
        if (m_capturer && !id.empty()) {
            const int ret = m_capturer->SetAudioSourceVolume(id, volume);
            if (ret != 0) {
                EmitLog(Format(L"%ls音量设置%ls", SlotName(slot), RetText(ret).c_str()));
            }
        }
    });
}

void LiveSession::SwitchResolution(std::string preset)
{
    Enqueue([this, preset = std::move(preset)] {
        if (!m_capturer) {
            return;
        }
        const int ret = m_capturer->SetResolutionPreset(preset);
        const SessionState st = m_state.load();
        const wchar_t* when = (st == SessionState::Live) ? L"，等待编码器切换完成的通知"
                                                           : L"，开播时生效（帧率保持界面上的设置）";
        EmitLog(Format(L"切换分辨率档位 %ls：%ls%ls", Utf8ToWide(preset).c_str(), RetText(ret).c_str(),
                       ret == 0 ? when : L""));
        if (ret == TGErr_NotSupported) {
            EmitLog(L"提示：GPU 性能模式下编码尺寸跟随采集，不能单独切换分辨率");
        }
    });
}

void LiveSession::SetPausePicture(std::string path)
{
    Enqueue([this, path = std::move(path)] {
        if (!m_capturer) {
            return;
        }
        PauseParams params;
        params.picture = path;
        const int ret = m_capturer->SetPauseInfo(params);
        if (ret == 0) {
            EmitLog(path.empty() ? L"暂停图片已清空：暂停时冻结最后一帧" : L"暂停图片已更新：" + Utf8ToWide(path));
        } else {
            EmitLog(Format(L"暂停图片加载失败 ret=%d（%ls）", ret, ErrorName(ret)));
        }
    });
}

void LiveSession::SetupWatermark(WatermarkParams params)
{
    Enqueue([this, params = std::move(params)] {
        if (!m_capturer) {
            return;
        }
        const int ret = m_capturer->SetupWatermarkInfo(params);
        const wchar_t* op = params.type == LogoOpType::LOGO_ADD      ? L"添加"
                            : params.type == LogoOpType::LOGO_UPDATE ? L"更新"
                                                                     : L"删除";
        EmitLog(Format(L"%lsLogo #%u：%ls（ret=%d）", op, params.index, ret >= 0 ? L"成功" : L"失败", ret));
    });
}

void LiveSession::StageOverlay(OverlayDesc desc)
{
    Enqueue([this, desc = std::move(desc)] {
        if (!m_capturer) {
            return;
        }
        // 视频层在暂存时就开始解码（探测文件要几十 ms），所以和其它 SDK 调用一样放在工作线程
        const int ret = m_capturer->StageOverlay(desc);
        EmitLog(Format(L"暂存叠加层 %ls（%ls x=%.0f%% y=%.0f%% 宽=%.0f%% 透明度=%.0f%% 层级=%d%ls）：%ls",
                       Utf8ToWide(desc.id).c_str(), desc.kind == OverlayKind::VideoFile ? L"视频" : L"图片",
                       desc.x * 100, desc.y * 100, desc.width * 100, desc.opacity * 100, desc.zOrder,
                       desc.visible ? L"" : L" 隐藏", RetText(ret).c_str()));
        EmitOverlays();
    });
}

void LiveSession::RemoveOverlay(std::string id)
{
    Enqueue([this, id = std::move(id)] {
        if (!m_capturer) {
            return;
        }
        const int ret = m_capturer->StageRemoveOverlay(id);
        EmitLog(Format(L"暂存移除叠加层 %ls：%ls", Utf8ToWide(id).c_str(), RetText(ret).c_str()));
        EmitOverlays();
    });
}

void LiveSession::CommitOverlays()
{
    Enqueue([this] {
        if (!m_capturer) {
            return;
        }
        const int ret = m_capturer->CommitOverlays();
        EmitLog(L"提交叠加层：" + RetText(ret));
        EmitOverlays();
    });
}

void LiveSession::DiscardOverlays()
{
    Enqueue([this] {
        if (!m_capturer) {
            return;
        }
        const int ret = m_capturer->DiscardOverlayChanges();
        EmitLog(L"撤销未提交的叠加层改动：" + RetText(ret));
        EmitOverlays();
    });
}

void LiveSession::RequestStats()
{
    if (m_statsPending.exchange(true)) {
        return;
    }
    Enqueue([this] {
        if (m_capturer && m_state.load() == SessionState::Live) {
            auto ev = std::make_unique<SessionEvent>();
            ev->type = SessionEvent::Type::Stats;
            ev->text = CollectStats();
            Emit(std::move(ev));
        }
        m_statsPending.store(false);
    });
}

void LiveSession::RequestLevels()
{
    if (m_levelsPending.exchange(true)) {
        return;
    }
    Enqueue([this] {
        if (m_capturer) {
            std::vector<AudioSourceLevel> levels;
            m_capturer->GetAudioLevels(levels);
            std::map<std::string, float> peak;
            for (const auto& l : levels) {
                peak[l.id] = l.peakDb;
            }
            auto ev = std::make_unique<SessionEvent>();
            ev->type = SessionEvent::Type::Levels;
            for (int i = 0; i < kAudioSlotCount; ++i) {
                const auto it = peak.find(m_slots[i].id);
                ev->levels[i] = (!m_slots[i].id.empty() && it != peak.end()) ? it->second : -100.0f;
            }
            const auto master = peak.find("master");
            ev->levels[kAudioSlotCount] = master != peak.end() ? master->second : -100.0f;
            Emit(std::move(ev));
        }
        m_levelsPending.store(false);
    });
}

// ------------------------------------------------------------------ 创建实例（工作线程）
// 视频源 + 视频编码参数（预览要用它确定采集尺寸 / 帧率）+ 运行期参数 + 勾选的音频源
bool LiveSession::CreateCapturer(const LiveConfig& cfg, int& code, std::wstring& why)
{
    m_capturer = ITGCapturer::Create();
    if (!m_capturer) {
        code = TGErr_Internal;
        why = L"ITGCapturer::Create 返回空";
        return false;
    }
    InstallCallbacks();

    // ① 视频源（Create*Capture 已把它设为主画面，AddVideoSource 是兼容老写法的显式绑定，重复调用无副作用）
    IVideoSource* video = nullptr;
    switch (cfg.source) {
        case VideoSourceKind::Monitor: video = m_capturer->CreateScreenCapture(cfg.monitorIndex, cfg.capMode); break;
        case VideoSourceKind::Window: video = m_capturer->CreateWindowCapture(cfg.window, cfg.capMode); break;
        case VideoSourceKind::File: video = m_capturer->CreateLocalFileCapture(0, cfg.videoFile, cfg.videoFileLoop); break;
    }
    if (!video) {
        code = TGErr_OpenVideoSource;
        why = cfg.source == VideoSourceKind::File ? L"本地视频文件不可用（打不开 / 没有 H.264 视频轨，详见日志）"
                                                  : L"创建视频采集源失败";
        return false;
    }
    if (const int ret = m_capturer->AddVideoSource(video); ret != 0) {
        code = ret;
        why = L"AddVideoSource 失败";
        return false;
    }

    // ② 视频编码参数：StartPreview 之前必须有（开播时会按界面上的最新参数再 Setup 一次）
    if (const int ret = m_capturer->SetupVideoEncoder(BuildVideoParams(cfg)); ret != 0) {
        code = ret;
        why = L"SetupVideoEncoder 参数非法";
        return false;
    }

    // ③ 运行期参数：断流阈值、暂停图片（依赖视频分辨率，必须在 SetupVideoEncoder 之后）
    m_capturer->SetAVTimeout(cfg.audioTimeoutMs, cfg.videoTimeoutMs);
    if (!cfg.pausePicture.empty()) {
        PauseParams pp;
        pp.picture = cfg.pausePicture;
        if (m_capturer->SetPauseInfo(pp) != 0) {
            EmitLog(L"警告：暂停图片加载失败，暂停时将冻结最后一帧");
        }
    }

    // ④ 音频源（打开失败不致命：取消勾选并继续；开播后打不开的设备由内核静音兜底并上报 AudioCaptureStalled）
    for (int i = 0; i < kAudioSlotCount; ++i) {
        const auto slot = static_cast<AudioSlot>(i);
        m_slots[i].muted = cfg.audio[i].muted;
        if (cfg.audio[i].enabled) {
            if (AddSlot(slot, cfg.audio[i]) != 0) {
                EmitLog(Format(L"警告：%ls添加失败，继续", SlotName(slot)));
                EmitSlot(slot, false);
            }
        }
    }
    if (cfg.source == VideoSourceKind::File && cfg.videoFileAudio) {
        AudioSourceDesc desc;
        desc.kind = AudioSourceKind::File;
        desc.id = "vfile";
        desc.filePath = cfg.videoFile;
        desc.loop = cfg.videoFileLoop;
        const int ret = m_capturer->AddAudioSource(desc);
        EmitLog(L"加入视频文件自带的音轨（id=vfile）：" + RetText(ret) +
                (ret == 0 ? L"" : L"。只支持 AAC 音轨；没有音轨的文件请取消该勾选"));
    }
    return true;
}

// ------------------------------------------------------------------ 预览（工作线程）
void LiveSession::DoStartPreview(const LiveConfig& cfg)
{
    if (m_previewOn.load()) {
        return;
    }
    const bool fresh = (m_capturer == nullptr);
    if (fresh) {
        int code = 0;
        std::wstring why;
        if (!CreateCapturer(cfg, code, why)) {
            EmitFailed(SessionEvent::Type::PreviewFailed, code, why);
            Teardown();
            return;
        }
    }
    const int ret = m_capturer->StartPreview();
    if (ret != 0) {
        EmitFailed(SessionEvent::Type::PreviewFailed, ret, L"StartPreview 失败");
        if (fresh) {
            Teardown();
        }
        return;
    }
    m_previewOn.store(true);
    EmitLog(L"预览已开启：预览画面显示暂存的叠加层，推流画面只含已提交的");
    SetState(m_state.load() == SessionState::Live ? SessionState::Live : SessionState::Previewing);
}

void LiveSession::DoStopPreview()
{
    if (!m_capturer || !m_previewOn.load()) {
        return;
    }
    // 预览回调在这里返回前已全部结束：之后界面可以放心清空预览区
    m_capturer->StopPreview();
    m_previewOn.store(false);
    if (m_state.load() == SessionState::Live) {
        EmitLog(L"预览已关闭（推流不受影响）");
        SetState(SessionState::Live);
    } else {
        EmitLog(L"预览已关闭，实例随之释放（叠加层场景一并清空）");
        Teardown();
    }
}

// ------------------------------------------------------------------ 开播（工作线程）
void LiveSession::DoStart(const LiveConfig& cfg)
{
    const SessionState before = m_state.load();
    if (before == SessionState::Live || before == SessionState::Connecting) {
        EmitLog(L"已在直播中，忽略重复的开始请求");
        return;
    }
    SetState(SessionState::Connecting);

    // ① 实例：预览中直接沿用（采集源、音频源、叠加层都已在上面），否则现建
    if (!m_capturer) {
        int code = 0;
        std::wstring why;
        if (!CreateCapturer(cfg, code, why)) {
            FailStart(code, why);
            return;
        }
    }

    // ② 编码与封装参数（三个 Setup 都必须成功，否则 Prepare 返回 NotConfigured）。
    //    预览中开播也重新 Setup 一次：码率 / 编码器 / 输出地址可能在预览期间改过
    m_liveHasAudio = cfg.HasAudio();
    AudioEncoderParams ap;
    ap.enabled = m_liveHasAudio;   // 开播时一路音频源都没有 = 纯视频推流
    ap.sampleRate = 44100;
    ap.channels = 2;
    ap.aBitrate = cfg.audioBitrateKbps * 1000;
    ap.aCodec = cfg.audioCodec;

    MuxerParams mp;
    mp.muxer = cfg.muxer;
    mp.muxerOutput = cfg.output;

    if (const int ret = m_capturer->SetupVideoEncoder(BuildVideoParams(cfg)); ret != 0) {
        FailStart(ret, L"SetupVideoEncoder 参数非法");
        return;
    }
    if (const int ret = m_capturer->SetupAudioEncoder(ap); ret != 0) {
        FailStart(ret, L"SetupAudioEncoder 参数非法");
        return;
    }
    if (const int ret = m_capturer->SetupMuxer(mp); ret != 0) {
        FailStart(ret, L"SetupMuxer 参数非法");
        return;
    }

    EmitLog(Format(L"开始连接：%ls  [%ls]  %ls %dx%d@%d  %ls / %ls", Utf8ToWide(cfg.output).c_str(),
                   Utf8ToWide(cfg.muxer).c_str(), Utf8ToWide(cfg.resolutionPreset).c_str(), cfg.width, cfg.height,
                   cfg.fps, Utf8ToWide(cfg.videoCodec).c_str(),
                   m_liveHasAudio ? Utf8ToWide(cfg.audioCodec).c_str() : L"无音频"));

    // ③ Prepare：建编码器、开采集、打开输出（可能阻塞到 connectTimeoutMs）。
    //    成功时在本线程同步回调 OnPrepared，我们在回调里调 Start()。
    m_startRet.store(0);
    if (const int ret = m_capturer->Prepare(); ret != 0) {
        FailStart(ret, L"Prepare 失败");
        return;
    }
    if (m_state.load() != SessionState::Live) {
        const int ret = m_startRet.load();
        FailStart(ret != 0 ? ret : TGErr_StartFailed, L"Start 失败");
        return;
    }
}

void LiveSession::FailStart(int code, std::wstring why)
{
    EmitFailed(SessionEvent::Type::StartFailed, code, std::move(why));
    if (m_capturer && m_previewOn.load()) {
        // Prepare 成功但 Start 失败时输出端已打开，Stop 把它收回；预览不中断
        m_capturer->Stop();
        SetState(SessionState::Previewing);
    } else {
        Teardown();
    }
}

void LiveSession::InstallCallbacks()
{
    m_capturer->SetOnPreparedCallback([this](ITGCapturer* c) {
        EmitLog(L"[回调] OnPrepared → 调用 Start()");
        m_startRet.store(c->Start());
    });
    m_capturer->SetOnStartCallback([this] {
        EmitLog(L"[回调] OnStart：开始推流 / 录制");
        SetState(SessionState::Live);
    });
    m_capturer->SetOnErrorCallback([this](int32_t what, int32_t extra, const char* msg) {
        auto ev = std::make_unique<SessionEvent>();
        ev->type = SessionEvent::Type::Error;
        ev->code = what;
        ev->extra = extra;
        ev->text = Utf8ToWide(msg ? msg : "");
        Emit(std::move(ev));
    });
    m_capturer->SetOnStopCallback([this](int32_t err) {
        EmitLog(Format(L"[回调] OnStop：err=%d（%ls）", err, ErrorName(err)));
    });
    m_capturer->SetOnNotifyCallback([this](NotifyMsgType type, const std::string& content, int64_t data) {
        EmitLog(Format(L"[通知] %ls  %ls  data=%lld", NotifyName(type), Utf8ToWide(content).c_str(),
                       static_cast<long long>(data)));
    });
    m_capturer->SetOnPreviewFrameCallback(m_previewCallback);
}

// ------------------------------------------------------------------ 音频源（工作线程）
std::set<std::string> LiveSession::AudioIds()
{
    std::vector<AudioSourceLevel> levels;
    m_capturer->GetAudioLevels(levels);
    std::set<std::string> ids;
    for (const auto& l : levels) {
        if (l.id != "master") {
            ids.insert(l.id);
        }
    }
    return ids;
}

int LiveSession::AddSlot(AudioSlot slot, const AudioSlotConfig& cfg)
{
    SlotRuntime& rt = m_slots[static_cast<int>(slot)];
    const bool speaker = (slot == AudioSlot::Speaker);
    int ret = 0;
    std::string id;
    std::wstring detail;
    if (slot == AudioSlot::File) {
        AudioSourceDesc desc;
        desc.kind = AudioSourceKind::File;
        desc.id = SlotId(slot);
        desc.filePath = cfg.filePath;
        desc.loop = cfg.loop;
        desc.volume = cfg.volume;
        ret = m_capturer->AddAudioSource(desc, &id);
        detail = Utf8ToWide(cfg.filePath) + (cfg.loop ? L"（循环）" : L"（播完静音）");
    } else if (cfg.deviceId.empty()) {
        // 系统默认设备：公开 API
        AudioSourceDesc desc;
        desc.kind = speaker ? AudioSourceKind::Speaker : AudioSourceKind::Microphone;
        desc.id = SlotId(slot);
        desc.volume = cfg.volume;
        ret = m_capturer->AddAudioSource(desc, &id);
        detail = L"系统默认设备";
    } else {
        // 指定设备：自建 WASAPI 源走老接口 AddAudioSource(IAudioSource*)。老接口不回 id，按前后差集找出来
        const std::set<std::string> before = AudioIds();
        auto source = std::make_shared<platform::WinAudioCapture>(speaker ? TypeOfSpeaker : TypeOfSelfMicphone,
                                                                  /*loopback*/ speaker);
        source->SetDeviceId(cfg.deviceId);
        ret = m_capturer->AddAudioSource(source.get());
        if (ret == 0) {
            for (const auto& x : AudioIds()) {
                if (!before.count(x)) {
                    id = x;
                }
            }
            rt.owned = std::move(source);
            m_capturer->SetAudioSourceVolume(id, cfg.volume);
        }
        detail = L"指定设备";
    }
    if (ret != 0) {
        EmitLog(Format(L"接入%ls（%ls）%ls", SlotName(slot), detail.c_str(), RetText(ret).c_str()));
        if (ret == TGErr_NotSupported && slot == AudioSlot::File) {
            EmitLog(L"提示：文件音频目前只支持 AAC 音轨");
        }
        return ret;
    }
    rt.id = id;
    if (rt.muted && slot != AudioSlot::File) {
        m_capturer->MuteAudio(true, speaker ? TypeOfSpeaker : TypeOfSelfMicphone);
    }
    EmitLog(Format(L"接入%ls（%ls）成功：id=%ls 音量 %.0f%%%ls", SlotName(slot), detail.c_str(),
                   Utf8ToWide(id).c_str(), cfg.volume * 100, rt.muted && slot != AudioSlot::File ? L"（静音）" : L""));
    return 0;
}

void LiveSession::RemoveSlot(AudioSlot slot)
{
    SlotRuntime& rt = m_slots[static_cast<int>(slot)];
    if (rt.id.empty()) {
        return;
    }
    const int ret = m_capturer->RemoveAudioSource(rt.id);
    EmitLog(Format(L"移除%ls（id=%ls）：%ls", SlotName(slot), Utf8ToWide(rt.id).c_str(), RetText(ret).c_str()));
    rt.id.clear();
    rt.owned.reset();   // RemoveAudioSource 返回时设备已关、回调已解绑
}

// ------------------------------------------------------------------ 停播（工作线程）
void LiveSession::DoStop()
{
    if (!m_capturer) {
        return;
    }
    const SessionState st = m_state.load();
    if (st != SessionState::Live && st != SessionState::Connecting) {
        return;
    }
    SetState(SessionState::Stopping);
    EmitLog(L"正在停止（排空编码器、写文件尾）…");
    m_capturer->Stop();   // 预览开着时内核回到 Previewing，采集不中断
    if (m_previewOn.load()) {
        EmitLog(L"已停止推流，预览继续（叠加层 / 音频源保留，可直接再次开播）");
        SetState(SessionState::Previewing);
    } else {
        Teardown();
        EmitLog(L"已停止");
    }
}

void LiveSession::Teardown()
{
    if (m_capturer) {
        if (m_previewOn.load()) {
            m_capturer->StopPreview();
        }
        m_capturer->Stop();
        ITGCapturer::Destroy(m_capturer);
        m_capturer = nullptr;
        EmitOverlays();   // 发空列表：界面清掉叠加层表
    }
    m_previewOn.store(false);
    for (auto& rt : m_slots) {
        rt.id.clear();
        rt.owned.reset();   // 自建音频源必须在 Destroy 之后释放
    }
    m_liveHasAudio = false;
    SetState(SessionState::Idle);
}

std::wstring LiveSession::CollectStats()
{
    const std::string br = m_capturer->GetStatsInfo(StatsInfoType::bitrateInfo);
    const std::string vi = m_capturer->GetStatsInfo(StatsInfoType::videoInfo);
    const std::string ai = m_capturer->GetStatsInfo(StatsInfoType::audioInfo);

    // 实时 = 1s 窗口（写出停滞 2s 归 0），用来看链路此刻是否健康；累计 = 自第一包起，用来核对产出量。
    // "封装后"含 flv/ts 封装开销，≈ 上行带宽 / 文件大小
    constexpr double kMB = 1024.0 * 1024.0;
    std::wstring text = Format(L"实时  %.0f kbps（封装后 %.0f kbps）    平均 %.0f kbps\r\n",
                               JsonNumber(br, "bitrate_bps") / 1000.0, JsonNumber(br, "io_bps") / 1000.0,
                               JsonNumber(br, "avg_bps") / 1000.0);
    text += Format(L"累计  %.2f MB（封装后 %.2f MB）    %.0f 包\r\n", JsonNumber(br, "bytes") / kMB,
                   JsonNumber(br, "io_bytes") / kMB, JsonNumber(br, "packets"));
    text += Format(L"视频 %ls  %.0fx%.0f  %.0f kbps / %.2f MB\r\n    已编码 %.0f 帧  丢帧 %.0f  积压 %.0f  耗时 %.1f ms\r\n",
                   Utf8ToWide(JsonString(vi, "codec")).c_str(), JsonNumber(vi, "width"), JsonNumber(vi, "height"),
                   JsonNumber(br, "video_bps") / 1000.0, JsonNumber(br, "video_bytes") / kMB, JsonNumber(vi, "encoded"),
                   JsonNumber(vi, "dropped"), JsonNumber(vi, "pending"), JsonNumber(vi, "latency_us") / 1000.0);
    const std::string acodec = JsonString(ai, "codec");
    if (acodec.empty()) {
        text += L"音频 未启用";
    } else {
        text += Format(L"音频 %ls  %.0f kbps / %.2f MB\r\n    已编码 %.0f 帧  积压 %.0f", Utf8ToWide(acodec).c_str(),
                       JsonNumber(br, "audio_bps") / 1000.0, JsonNumber(br, "audio_bytes") / kMB,
                       JsonNumber(ai, "encoded"), JsonNumber(ai, "pending"));
    }
    return text;
}

// ------------------------------------------------------------------ 事件投递
void LiveSession::SetState(SessionState s)
{
    // 不去重：预览开关时状态本身可能不变（例如直播中开 / 关预览），界面也要据此刷新
    m_state.store(s);
    auto ev = std::make_unique<SessionEvent>();
    ev->type = SessionEvent::Type::StateChanged;
    ev->state = s;
    ev->preview = m_previewOn.load();
    Emit(std::move(ev));
}

void LiveSession::Emit(std::unique_ptr<SessionEvent> ev)
{
    SessionEvent* raw = ev.release();
    if (!PostMessageW(m_notifyWnd, WM_SESSION_EVENT, 0, reinterpret_cast<LPARAM>(raw))) {
        delete raw;   // 窗口已销毁（退出过程中），丢弃即可
    }
}

void LiveSession::EmitLog(std::wstring text)
{
    auto ev = std::make_unique<SessionEvent>();
    ev->type = SessionEvent::Type::Log;
    ev->text = std::move(text);
    Emit(std::move(ev));
}

void LiveSession::EmitFailed(SessionEvent::Type type, int code, std::wstring why)
{
    auto ev = std::make_unique<SessionEvent>();
    ev->type = type;
    ev->code = code;
    ev->text = std::move(why);
    Emit(std::move(ev));
}

void LiveSession::EmitSlot(AudioSlot slot, bool active)
{
    auto ev = std::make_unique<SessionEvent>();
    ev->type = SessionEvent::Type::AudioSlotChanged;
    ev->code = static_cast<int>(slot);
    ev->extra = active ? 1 : 0;
    Emit(std::move(ev));
}

void LiveSession::EmitOverlays()
{
    auto ev = std::make_unique<SessionEvent>();
    ev->type = SessionEvent::Type::Overlays;
    if (m_capturer) {
        m_capturer->GetOverlays(ev->committed, false);
        m_capturer->GetOverlays(ev->staged, true);
    }
    Emit(std::move(ev));
}

}  // namespace tgui
