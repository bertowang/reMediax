// 开播会话：封装 ITGCapturer 的完整生命周期，是本 demo 里唯一直接调用 SDK 的地方。
//
// 线程模型（接入方可直接照搬）
//   - 所有 SDK 调用都在一条专用工作线程上**串行**执行：
//       * Prepare 要连接推流服务器，最长可阻塞 MuxerParams::connectTimeoutMs（默认 10s）；
//         Stop 要等编码器排空、写文件尾。放在界面线程会让窗口"未响应"。
//       * 串行化也让 capturer 指针只被一条线程访问，不需要额外加锁。
//   - SDK 回调（可能来自内核任意线程）和执行结果，一律 PostMessage(WM_SESSION_EVENT) 投递回界面线程；
//     lParam 是 new 出来的 SessionEvent*，由界面线程接管并释放。回调里绝不弹框、不阻塞。
#ifndef TGUI_LIVE_SESSION_H
#define TGUI_LIVE_SESSION_H

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LiveConfig.h"
#include "TGCapture/ITGCapturer.h"

namespace tgui
{

constexpr UINT WM_SESSION_EVENT = WM_APP + 1;

enum class SessionState
{
    Idle,         // 未开播
    Connecting,   // Create → Setup* → Prepare（连接推流地址 / 打开文件）
    Live,         // 已收到 OnStart
    Stopping,     // Stop + Destroy 中
};

struct SessionEvent
{
    enum class Type
    {
        StateChanged,    // state
        Log,             // text
        Error,           // SDK OnErrorCallback：code = what，extra = 底层原始码，text = 描述
        StartFailed,     // 开播失败（已自动回滚到 Idle）：code = 错误码，text = 失败环节
        Stats,           // text = 已格式化的统计
        EncodersProbed,  // videoEncoders / audioEncoders = 逗号分隔的可用编码器
    };

    Type type = Type::Log;
    SessionState state = SessionState::Idle;
    int code = 0;
    int extra = 0;
    std::wstring text;
    std::string videoEncoders;
    std::string audioEncoders;
};

// TGCaptureError → 中文说明（界面提示用）
const wchar_t* ErrorName(int code);

// 运行期致命错误：出现后同一连接不会再恢复（FFmpeg I/O 错误是粘性的），宿主应停播
bool IsFatalRuntimeError(int code);

class LiveSession
{
public:
    explicit LiveSession(HWND notifyWnd);
    ~LiveSession();

    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    // 以下接口都只是把任务投递到工作线程，立即返回
    void ProbeEncoders();
    void Start(LiveConfig cfg);
    void Stop();
    void MuteVideo(bool mute);
    void MuteAudio(TGCapture::AudioCapSourceType channel, bool mute);
    void SetPausePicture(std::string path);
    void SetupWatermark(TGCapture::WatermarkParams params);
    void RequestStats();   // 上一次还没取完时直接忽略，避免停播阻塞期间请求堆积

    // 停播并等待工作线程退出（同步）。退出程序时调用，之后不再有任何 WM_SESSION_EVENT
    void Shutdown();

private:
    void Enqueue(std::function<void()> task);
    void WorkerLoop();

    void DoStart(const LiveConfig& cfg);
    void DoStop();
    void Teardown();                   // Stop + Destroy + 释放自建音频源，回到 Idle
    int AddAudio(TGCapture::AudioCapSourceType type, const std::wstring& deviceId);
    void InstallCallbacks();
    std::wstring CollectStats();

    void SetState(SessionState s);
    void Emit(std::unique_ptr<SessionEvent> ev);
    void EmitLog(std::wstring text);
    void EmitStartFailed(int code, std::wstring why);

    HWND m_notifyWnd = nullptr;

    // ---- 仅工作线程访问 ----
    TGCapture::ITGCapturer* m_capturer = nullptr;
    // 指定了具体设备的音频源由本 demo 自建（公开 API 的 Enable*Capture 只用系统默认设备），
    // 必须比 capturer 活得久：Teardown 里先 Destroy capturer 再释放这里
    std::vector<std::shared_ptr<TGCapture::IAudioSource>> m_ownedAudio;

    std::atomic<SessionState> m_state{SessionState::Idle};
    std::atomic<int> m_startRet{0};
    std::atomic<bool> m_statsPending{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    bool m_quit = false;
    std::thread m_worker;
};

}  // namespace tgui

#endif  // TGUI_LIVE_SESSION_H
