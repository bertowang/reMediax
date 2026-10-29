// 采集源抽象：平台实现继承后按帧产出，帧以 shared_ptr 交给总线
#ifndef TGCAPTURE_CAPTURE_SOURCE_H
#define TGCAPTURE_CAPTURE_SOURCE_H

#include <atomic>
#include <functional>
#include <memory>

#include "TGCapture/TGCapturerInterface.h"
#include "core/EmitGate.h"
#include "core/Frame.h"
#include "core/Logger.h"
#include "core/Thread.h"

namespace TGCapture::capture
{

// 采集回调栅栏：实现已通用化到 core/EmitGate.h（总线节点的受控输出也用它），原理见该文件。
// 所有平台的帧最终都经过基类 Emit()，所以在这里统一设闸，不需要改任何平台实现。
using EmitGate = core::EmitGate;

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

    // 运行期改采集尺寸（已 Open，可在采集中调用）。返回 false = 本源按原生尺寸采集、不支持改尺寸，
    // 由编码器缩放（Windows 桌面复制 / iOS ReplayKit 都是这种）。只有输出尺寸本来就由请求决定的源才覆写
    virtual bool Resize(int /*width*/, int /*height*/) { return false; }

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
