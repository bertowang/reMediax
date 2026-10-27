// 采集源抽象：平台实现继承后按帧产出，帧以 shared_ptr 交给总线
#ifndef TGCAPTURE_CAPTURE_SOURCE_H
#define TGCAPTURE_CAPTURE_SOURCE_H

#include <atomic>
#include <functional>
#include <memory>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Frame.h"
#include "core/Logger.h"
#include "core/Thread.h"

namespace TGCapture::capture
{

// 采集回调栅栏。
//
// 要解决的问题：各平台产帧的线程并不都是我们自己起的 —— Android 的 PCM/图像由 Java 线程回调、
// iOS ReplayKit 由系统队列回调、外灌源由宿主线程直接推。这些路径上 Worker::Stop() 的 join
// 只停了"保活线程"，并不构成回调栅栏。而 TGCapturer::StopLocked 默认"StopCapture 返回即无回调"，
// 随后就去 Close 编码器 / Reset 混流器 —— 在飞的回调会踩到正在拆除的对象。
//
// 所有平台的帧最终都经过基类 Emit()，所以在这里统一设闸，不需要改任何平台实现。
//
// 关闸与计数的双向检查（Dekker 式，依赖 seq_cst）：
//   关闭侧：Close() 置 false  →  读 m_inFlight
//   通过侧：m_inFlight +1     →  再读一次 m_open
// 若通过侧第二次读到 true，说明它的 +1 排在 Close() 之前，关闭侧读计数时必然看到 ≥1 而等待；
// 若读到 false，则回调被丢弃。两种情况都不会出现"关闭侧已返回、回调仍在跑"。
class EmitGate
{
public:
    void Open() noexcept { m_open.store(true); }
    void Close() noexcept { m_open.store(false); }

    // 返回 true 表示允许触发回调，调用方必须配对调用 Leave()
    bool Enter() noexcept
    {
        if (!m_open.load()) {
            return false;
        }
        m_inFlight.fetch_add(1);
        if (!m_open.load()) {
            m_inFlight.fetch_sub(1);
            return false;
        }
        return true;
    }

    void Leave() noexcept { m_inFlight.fetch_sub(1); }

    // 等待在飞回调退出。自旋而不是条件变量：这里等的是"回调函数体执行完"，
    // 正常情况下是微秒级，且停机路径本就不在热路径上。
    void WaitDrained() const noexcept
    {
        while (m_inFlight.load() > 0) {
            std::this_thread::yield();
        }
    }

private:
    // 默认开闸：没有显式调用 StartCapture 的路径（如仅 Open 后由宿主直接推帧）保持原有行为
    std::atomic_bool m_open{true};
    std::atomic<int> m_inFlight{0};
};

// 视频采集源：平台实现周期性调用 Emit(frame)
class VideoSourceBase : public IVideoSource, public core::Worker
{
public:
    VideoSourceBase(VideoCapSourceType type, VideoScaleStrategy strategy)
        : IVideoSource(type, strategy), core::Worker("video_capture") {}

    using EmitCallback = std::function<void(core::FramePtr)>;

    void SetEmitCallback(EmitCallback cb) { m_emit = std::move(cb); }

    // 打开由平台实现
    virtual int Open(int width, int height, float fps) = 0;

    // 释放设备资源（句柄 / COM 对象 / D3D 资源 / 系统缓冲）。
    // 默认空实现：没有需要提前释放资源的平台保持原样（资源随析构回收）。
    // 必须幂等，且 Close() 之后再 Open() 要能重新工作 —— Prepare 失败回滚与 Stop 都会调它。
    virtual void Close() {}

    // 与 StopCapture 对称：先开闸再起线程。
    // 开闸必须在起线程之前，否则刚产出的头几帧会被上一轮残留的关闸状态丢掉。
    bool StartCapture()
    {
        m_gate.Open();
        return Start();
    }

    void StopCapture()
    {
        // 三步缺一不可：关闸挡住后续回调 → join 自有线程 → 等在飞的回调退出
        m_gate.Close();
        Stop();
        m_gate.WaitDrained();
    }

    [[nodiscard]] bool IsCapturing() const noexcept { return IsRunning(); }

    // 采集实际输出的尺寸（Open 之后有效）
    [[nodiscard]] int FrameWidth() const noexcept { return m_width; }
    [[nodiscard]] int FrameHeight() const noexcept { return m_height; }
    [[nodiscard]] uint64_t EmittedFrames() const noexcept { return m_emittedFrames.load(); }

    // 该源能否把帧留在显存直送硬编（GPU 模式的前提）。
    // 默认 false：只有真正支持硬件帧直通的平台实现才覆写。
    // 关键：不能靠"是不是某个平台类"来判断 —— 同一个类（如 WinScreenCapture）
    // 在屏幕与窗口两种模式下能力不同，窗口模式没有桌面复制通道，强行走 GPU 会抓到整屏。
    [[nodiscard]] virtual bool SupportsGpuDirect() const noexcept { return false; }

protected:
    void Emit(core::FramePtr frame)
    {
        if (!frame || !m_gate.Enter()) {
            return;
        }
        if (m_emit) {
            m_emittedFrames.fetch_add(1);
            m_emit(std::move(frame));
        }
        m_gate.Leave();
    }

    void WaitEmitDrained() const noexcept { m_gate.WaitDrained(); }

    // 默认退出摘要：平台实现可覆写拼接更多字段
    void OnThreadExit() override
    {
        TG_LOGI("VideoSource[%d]: 线程退出 emitted=%llu size=%dx%d fps=%.1f",
                static_cast<int>(GetVideoCapSourceType()),
                static_cast<unsigned long long>(m_emittedFrames.load()), m_width, m_height, m_fps);
    }

    int m_width = 0;
    int m_height = 0;
    float m_fps = 30.0f;
    std::atomic<uint64_t> m_emittedFrames{0};

private:
    EmitCallback m_emit;
    mutable EmitGate m_gate;
};

// 音频采集源：产出交错 S16 PCM
class AudioSourceBase : public IAudioSource, public core::Worker
{
public:
    AudioSourceBase(AudioCapSourceType type) : IAudioSource(type), core::Worker("audio_capture") {}

    using EmitCallback = std::function<void(core::FramePtr)>;

    void SetEmitCallback(EmitCallback cb) { m_emit = std::move(cb); }

    virtual int Open(int sampleRate, int channels) = 0;

    // 同 VideoSourceBase::Close()：默认空实现，需要提前放设备的平台覆写，必须幂等且可重新 Open
    virtual void Close() {}

    bool StartCapture()
    {
        m_gate.Open();
        return Start();
    }

    void StopCapture()
    {
        m_gate.Close();
        Stop();
        m_gate.WaitDrained();
    }

    [[nodiscard]] bool IsCapturing() const noexcept { return IsRunning(); }
    [[nodiscard]] uint64_t EmittedFrames() const noexcept { return m_emittedFrames.load(); }

    // 静音：对齐 MediaX "保留帧节拍 + PCM 清零"，PTS 继续推进不影响下游。
    // Win/iOS/Android 采集 Loop 都已读取 m_muted，这里只把开关对外暴露给门面。
    void SetMuted(bool mute) noexcept { m_muted.store(mute); }
    [[nodiscard]] bool IsMuted() const noexcept { return m_muted.load(); }

protected:
    void Emit(core::FramePtr frame)
    {
        if (!frame || !m_gate.Enter()) {
            return;
        }
        if (m_emit) {
            m_emittedFrames.fetch_add(1);
            m_emit(std::move(frame));
        }
        m_gate.Leave();
    }

    void WaitEmitDrained() const noexcept { m_gate.WaitDrained(); }

    void OnThreadExit() override
    {
        TG_LOGI("AudioSource[%d]: 线程退出 emitted=%llu %dHz %dch muted=%d",
                static_cast<int>(GetAudioCapSourceType()),
                static_cast<unsigned long long>(m_emittedFrames.load()),
                m_sampleRate, m_channels, m_muted.load() ? 1 : 0);
    }

    int m_sampleRate = 44100;
    int m_channels = 2;
    std::atomic_bool m_muted{false};
    std::atomic<uint64_t> m_emittedFrames{0};

private:
    EmitCallback m_emit;
    mutable EmitGate m_gate;
};

}  // namespace TGCapture::capture

#endif  // TGCAPTURE_CAPTURE_SOURCE_H
