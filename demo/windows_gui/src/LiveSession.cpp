#include "LiveSession.h"

#include <cstdlib>

#include "StringUtil.h"
// 指定音频端点时直接用平台层的 WASAPI 源（与命令行 demo 相同做法）；
// 只用系统默认设备的宿主走公开 API EnableMicrophoneCapture / EnableSpeakerCapture 即可，无需包含它
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
        case EsCap_Notify_VideoMetaInfo_Changed: return L"视频参数变化";
        case EsCap_Notify_AudioMetaInfo_Changed: return L"音频参数变化";
        case EsCap_Notify_SystemFreeze: return L"检测到系统卡顿 / 挂起";
        case EsCap_Notify_LocalFile_EOF: return L"本地文件播放结束";
        case EsCap_Notify_VideoCapture_Recovered: return L"视频采集已恢复";
        case EsCap_Notify_AudioCapture_Recovered: return L"音频采集已恢复";
        case EsCap_Notify_Encoder_Fallback: return L"编码器回落";
        default: return L"通知";
    }
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

}  // namespace

// ------------------------------------------------------------------ 构造 / 任务队列
LiveSession::LiveSession(HWND notifyWnd) : m_notifyWnd(notifyWnd)
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
    Enqueue([this] { DoStop(); });
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
                break;   // m_quit 且队列已排空：Shutdown 投递的 DoStop 一定已执行
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

void LiveSession::MuteAudio(AudioCapSourceType channel, bool mute)
{
    Enqueue([this, channel, mute] {
        if (m_capturer) {
            const bool applied = m_capturer->MuteAudio(mute, channel);
            EmitLog(Format(L"%ls%ls%ls", channel == TypeOfSpeaker ? L"系统声音" : L"麦克风",
                           mute ? L"已静音" : L"已取消静音", applied ? L"" : L"（该路未开启，未生效）"));
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

// ------------------------------------------------------------------ 开播（工作线程）
void LiveSession::DoStart(const LiveConfig& cfg)
{
    if (m_capturer) {
        EmitLog(L"已在直播中，忽略重复的开始请求");
        return;
    }
    SetState(SessionState::Connecting);

    // ① 创建实例
    m_capturer = ITGCapturer::Create();
    if (!m_capturer) {
        EmitStartFailed(TGErr_Internal, L"ITGCapturer::Create 返回空");
        Teardown();
        return;
    }
    InstallCallbacks();

    // ② 视频源
    IVideoSource* video = cfg.captureWindow
                              ? m_capturer->CreateWindowCapture(cfg.window, cfg.capMode)
                              : m_capturer->CreateScreenCapture(cfg.monitorIndex, cfg.capMode);
    if (!video) {
        EmitStartFailed(TGErr_OpenVideoSource, L"创建视频采集源失败");
        Teardown();
        return;
    }
    if (const int ret = m_capturer->AddVideoSource(video); ret != 0) {
        EmitStartFailed(ret, L"AddVideoSource 失败");
        Teardown();
        return;
    }

    // ③ 音频源（打开失败不致命：内核会用静音兜底，并上报 AudioCaptureStalled）
    const bool hasAudio = cfg.micEnabled || cfg.speakerEnabled;
    if (cfg.micEnabled && AddAudio(TypeOfSelfMicphone, cfg.micDeviceId) != 0) {
        EmitLog(L"警告：麦克风添加失败，继续开播");
    }
    if (cfg.speakerEnabled && AddAudio(TypeOfSpeaker, cfg.speakerDeviceId) != 0) {
        EmitLog(L"警告：系统声音添加失败，继续开播");
    }

    // ④ 编码与封装参数（三个 Setup 都必须成功，否则 Prepare 返回 NotConfigured）
    VideoEncoderParams vp;
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

    AudioEncoderParams ap;
    ap.enabled = hasAudio;   // 两路都没开 = 纯视频推流
    ap.sampleRate = 44100;
    ap.channels = 2;
    ap.aBitrate = cfg.audioBitrateKbps * 1000;
    ap.aCodec = cfg.audioCodec;

    MuxerParams mp;
    mp.muxer = cfg.muxer;
    mp.muxerOutput = cfg.output;

    if (const int ret = m_capturer->SetupVideoEncoder(vp); ret != 0) {
        EmitStartFailed(ret, L"SetupVideoEncoder 参数非法");
        Teardown();
        return;
    }
    if (const int ret = m_capturer->SetupAudioEncoder(ap); ret != 0) {
        EmitStartFailed(ret, L"SetupAudioEncoder 参数非法");
        Teardown();
        return;
    }
    if (const int ret = m_capturer->SetupMuxer(mp); ret != 0) {
        EmitStartFailed(ret, L"SetupMuxer 参数非法");
        Teardown();
        return;
    }

    // ⑤ 运行期参数：断流阈值、暂停图片（依赖视频分辨率，必须在 SetupVideoEncoder 之后）
    m_capturer->SetAVTimeout(cfg.audioTimeoutMs, cfg.videoTimeoutMs);
    if (!cfg.pausePicture.empty()) {
        PauseParams pp;
        pp.picture = cfg.pausePicture;
        if (m_capturer->SetPauseInfo(pp) != 0) {
            EmitLog(L"警告：暂停图片加载失败，暂停时将冻结最后一帧");
        }
    }

    EmitLog(Format(L"开始连接：%ls  [%ls]  %dx%d@%d  %ls / %ls", Utf8ToWide(cfg.output).c_str(),
                   Utf8ToWide(cfg.muxer).c_str(), cfg.width, cfg.height, cfg.fps,
                   Utf8ToWide(cfg.videoCodec).c_str(), hasAudio ? Utf8ToWide(cfg.audioCodec).c_str() : L"无音频"));

    // ⑥ Prepare：建编码器、开采集、打开输出（可能阻塞到 connectTimeoutMs）。
    //    成功时在本线程同步回调 OnPrepared，我们在回调里调 Start()。
    m_startRet.store(0);
    if (const int ret = m_capturer->Prepare(); ret != 0) {
        EmitStartFailed(ret, L"Prepare 失败");
        Teardown();
        return;
    }
    if (m_state.load() != SessionState::Live) {
        const int ret = m_startRet.load();
        EmitStartFailed(ret != 0 ? ret : TGErr_StartFailed, L"Start 失败");
        Teardown();
        return;
    }

    // ⑦ 初始静音状态（源已就绪后再设，保证生效）
    if (cfg.micEnabled && cfg.micMuted) {
        m_capturer->MuteAudio(true, TypeOfSelfMicphone);
    }
    if (cfg.speakerEnabled && cfg.speakerMuted) {
        m_capturer->MuteAudio(true, TypeOfSpeaker);
    }
}

int LiveSession::AddAudio(AudioCapSourceType type, const std::wstring& deviceId)
{
    const bool speaker = (type == TypeOfSpeaker);
    if (deviceId.empty()) {
        return speaker ? m_capturer->EnableSpeakerCapture() : m_capturer->EnableMicrophoneCapture();
    }
    auto source = std::make_shared<platform::WinAudioCapture>(type, /*loopback*/ speaker);
    source->SetDeviceId(deviceId);
    const int ret = m_capturer->AddAudioSource(source.get());
    if (ret == 0) {
        m_ownedAudio.push_back(std::move(source));
    }
    return ret;
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
}

// ------------------------------------------------------------------ 停播（工作线程）
void LiveSession::DoStop()
{
    if (!m_capturer) {
        return;
    }
    SetState(SessionState::Stopping);
    EmitLog(L"正在停止（排空编码器、写文件尾）…");
    Teardown();
    EmitLog(L"已停止");
}

void LiveSession::Teardown()
{
    if (m_capturer) {
        m_capturer->Stop();
        ITGCapturer::Destroy(m_capturer);
        m_capturer = nullptr;
    }
    m_ownedAudio.clear();
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
    text += Format(L"视频 %ls  %.0f kbps / %.2f MB    已编码 %.0f 帧  丢帧 %.0f  积压 %.0f  耗时 %.1f ms\r\n",
                   Utf8ToWide(JsonString(vi, "codec")).c_str(), JsonNumber(br, "video_bps") / 1000.0,
                   JsonNumber(br, "video_bytes") / kMB, JsonNumber(vi, "encoded"), JsonNumber(vi, "dropped"),
                   JsonNumber(vi, "pending"), JsonNumber(vi, "latency_us") / 1000.0);
    const std::string acodec = JsonString(ai, "codec");
    if (acodec.empty()) {
        text += L"音频 未启用";
    } else {
        text += Format(L"音频 %ls  %.0f kbps / %.2f MB    已编码 %.0f 帧  积压 %.0f", Utf8ToWide(acodec).c_str(),
                       JsonNumber(br, "audio_bps") / 1000.0, JsonNumber(br, "audio_bytes") / kMB,
                       JsonNumber(ai, "encoded"), JsonNumber(ai, "pending"));
    }
    return text;
}

// ------------------------------------------------------------------ 事件投递
void LiveSession::SetState(SessionState s)
{
    if (m_state.exchange(s) == s) {
        return;
    }
    auto ev = std::make_unique<SessionEvent>();
    ev->type = SessionEvent::Type::StateChanged;
    ev->state = s;
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

void LiveSession::EmitStartFailed(int code, std::wstring why)
{
    auto ev = std::make_unique<SessionEvent>();
    ev->type = SessionEvent::Type::StartFailed;
    ev->code = code;
    ev->text = std::move(why);
    Emit(std::move(ev));
}

}  // namespace tgui
