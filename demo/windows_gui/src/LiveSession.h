// 开播会话：封装 ITGCapturer 的完整生命周期，是本 demo 里唯一直接调用 SDK 的地方。
//
// 线程模型（接入方可直接照搬）
//   - 所有 SDK 调用都在一条专用工作线程上**串行**执行：
//       * Prepare 要连接推流服务器，最长可阻塞 MuxerParams::connectTimeoutMs（默认 10s）；
//         Stop 要等编码器排空、写文件尾。放在界面线程会让窗口"未响应"。
//       * 串行化也让 capturer 指针只被一条线程访问，不需要额外加锁。
//   - SDK 回调（可能来自内核任意线程）和执行结果，一律 PostMessage(WM_SESSION_EVENT) 投递回界面线程；
//     lParam 是 new 出来的 SessionEvent*，由界面线程接管并释放。回调里绝不弹框、不阻塞。
//   - 预览帧回调例外：它在内核的预览投递线程上直接交给 PreviewView（转换 + 缩放后 InvalidateRect），
//     不经过工作线程，也不经过消息队列（每秒几十帧，排队只会积压）。
//
// 实例生命周期
//   capturer 在「开启预览」或「开始直播」时创建，预览和推流都停了才销毁。叠加层场景、运行期增删的音频源
//   都挂在实例上：预览中摆好叠加层再开播，停播后预览还开着时场景保留；两者都关掉后随实例一起释放。
#ifndef TGUI_LIVE_SESSION_H
#define TGUI_LIVE_SESSION_H

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "LiveConfig.h"
#include "TGCapture/ITGCapturer.h"

namespace TGCapture::platform
{
class WinAudioCapture;
}

namespace tgui
{

constexpr UINT WM_SESSION_EVENT = WM_APP + 1;

enum class SessionState
{
    Idle,         // 没有实例
    Previewing,   // 只开了预览，未推流
    Connecting,   // Setup* → Prepare（连接推流地址 / 打开文件）
    Live,         // 已收到 OnStart（预览可开可关，见 SessionEvent::preview）
    Stopping,     // Stop（+ Destroy）中
};

struct SessionEvent
{
    enum class Type
    {
        StateChanged,       // state + preview（预览是否开着）
        Log,                // text
        Error,              // SDK OnErrorCallback：code = what，extra = 底层原始码，text = 描述
        StartFailed,        // 开播失败（已回滚：预览开着时回到预览，否则销毁实例）：code = 错误码，text = 失败环节
        PreviewFailed,      // 开启预览失败：code = 错误码，text = 失败环节
        Stats,              // text = 已格式化的统计
        EncodersProbed,     // videoEncoders / audioEncoders = 逗号分隔的可用编码器
        Levels,             // levels[AudioSlot] + levels[kAudioSlotCount] = 混音输出；峰值 dBFS，-100 = 无 / 静音
        AudioSlotChanged,   // code = AudioSlot，extra = 1 已接入 / 0 已移除（或接入失败）
        Overlays,           // committed / staged：两份叠加层列表（按绘制顺序）
    };

    Type type = Type::Log;
    SessionState state = SessionState::Idle;
    bool preview = false;
    int code = 0;
    int extra = 0;
    std::wstring text;
    std::string videoEncoders;
    std::string audioEncoders;
    float levels[kAudioSlotCount + 1] = {-100.0f, -100.0f, -100.0f, -100.0f};
    std::vector<TGCapture::OverlayDesc> committed;
    std::vector<TGCapture::OverlayDesc> staged;
};

// TGCaptureError → 中文说明（界面提示用）
const wchar_t* ErrorName(int code);

// 运行期致命错误：出现后同一连接不会再恢复（FFmpeg I/O 错误是粘性的），宿主应停播
bool IsFatalRuntimeError(int code);

class LiveSession
{
public:
    // previewCallback：预览帧回调，在内核预览投递线程上调用（见文件头）
    LiveSession(HWND notifyWnd, TGCapture::ITGCapturer::OnPreviewFrameCallback previewCallback);
    ~LiveSession();

    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    // 以下接口都只是把任务投递到工作线程，立即返回
    void ProbeEncoders();
    void StartPreview(LiveConfig cfg);   // 没有实例时按 cfg 创建（视频源 / 音频源 / 视频编码参数）
    void StopPreview();                  // 未推流时连同实例一起销毁
    void Start(LiveConfig cfg);          // 预览中开播：只重新 Setup 编码 / 封装参数，采集源沿用预览时的
    void Stop();                         // 预览开着时回到预览
    void MuteVideo(bool mute);
    void MuteAudio(AudioSlot slot, bool mute);
    // 运行期开关一路音频源：enabled = true 接入（已接入则按新设备 / 文件重接），false 移除。没有实例时忽略
    void SetAudioSlot(AudioSlot slot, AudioSlotConfig cfg);
    void SetAudioVolume(AudioSlot slot, float volume);
    void SwitchResolution(std::string preset);
    void SetPausePicture(std::string path);
    void SetupWatermark(TGCapture::WatermarkParams params);
    void StageOverlay(TGCapture::OverlayDesc desc);
    void RemoveOverlay(std::string id);
    void CommitOverlays();
    void DiscardOverlays();
    void RequestStats();    // 上一次还没取完时直接忽略，避免停播阻塞期间请求堆积
    void RequestLevels();   // 同上

    // 停播并等待工作线程退出（同步）。退出程序时调用，之后不再有任何 WM_SESSION_EVENT
    void Shutdown();

private:
    struct SlotRuntime
    {
        std::string id;   // SDK 里的音频源 id；空 = 未接入
        // 指定了具体设备的麦克风 / 扬声器由本 demo 自建（AudioSourceDesc 不能指定设备），
        // 必须比它在 capturer 里的节点活得久：RemoveAudioSource / Destroy 之后才释放
        std::shared_ptr<TGCapture::platform::WinAudioCapture> owned;
        bool muted = false;
    };

    void Enqueue(std::function<void()> task);
    void WorkerLoop();

    void DoStartPreview(const LiveConfig& cfg);
    void DoStopPreview();
    void DoStart(const LiveConfig& cfg);
    void DoStop();
    void FailStart(int code, std::wstring why);
    bool CreateCapturer(const LiveConfig& cfg, int& code, std::wstring& why);
    void Teardown();   // Stop + Destroy + 释放自建音频源，回到 Idle
    int AddSlot(AudioSlot slot, const AudioSlotConfig& cfg);
    void RemoveSlot(AudioSlot slot);
    std::set<std::string> AudioIds();
    void InstallCallbacks();
    std::wstring CollectStats();

    void SetState(SessionState s);
    void Emit(std::unique_ptr<SessionEvent> ev);
    void EmitLog(std::wstring text);
    void EmitFailed(SessionEvent::Type type, int code, std::wstring why);
    void EmitSlot(AudioSlot slot, bool active);
    void EmitOverlays();

    HWND m_notifyWnd = nullptr;
    TGCapture::ITGCapturer::OnPreviewFrameCallback m_previewCallback;

    // ---- 仅工作线程访问 ----
    TGCapture::ITGCapturer* m_capturer = nullptr;
    SlotRuntime m_slots[kAudioSlotCount];
    bool m_liveHasAudio = false;   // 本轮推流是否带音轨（不带时运行期不能再加音频源）

    std::atomic<SessionState> m_state{SessionState::Idle};
    std::atomic<bool> m_previewOn{false};
    std::atomic<int> m_startRet{0};
    std::atomic<bool> m_statsPending{false};
    std::atomic<bool> m_levelsPending{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    bool m_quit = false;
    std::thread m_worker;
};

}  // namespace tgui

#endif  // TGUI_LIVE_SESSION_H
