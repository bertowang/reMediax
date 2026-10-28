// 回调栅栏（EmitGate）
//
// 要解决的问题：各平台产帧的线程并不都是我们自己起的 —— Android 的 PCM/图像由 Java 线程回调、
// iOS ReplayKit 由系统队列回调、外灌源由宿主线程直接推。这些路径上 Worker::Stop() 的 join
// 只停了"保活线程"，并不构成回调栅栏。而停机流程默认"停止返回即无回调"，
// 随后就去 Close 编码器 / Reset 混流器 —— 在飞的回调会踩到正在拆除的对象。
//
// 用法：产出方每次回调前 Enter()，成功才回调、回调后 Leave()；停止方 Close() → 停线程 → WaitDrained()。
// 采集源基类（capture::VideoSourceBase / AudioSourceBase）与总线节点（BusNode 的受控输出）共用。
//
// 关闸与计数的双向检查（Dekker 式，依赖 seq_cst）：
//   关闭侧：Close() 置 false  →  读 m_inFlight
//   通过侧：m_inFlight +1     →  再读一次 m_open
// 若通过侧第二次读到 true，说明它的 +1 排在 Close() 之前，关闭侧读计数时必然看到 ≥1 而等待；
// 若读到 false，则回调被丢弃。两种情况都不会出现"关闭侧已返回、回调仍在跑"。
#ifndef TGCAPTURE_CORE_EMIT_GATE_H
#define TGCAPTURE_CORE_EMIT_GATE_H

#include <atomic>
#include <thread>

namespace TGCapture::core
{

class EmitGate
{
public:
    void Open() noexcept { m_open.store(true); }
    void Close() noexcept { m_open.store(false); }
    [[nodiscard]] bool IsOpen() const noexcept { return m_open.load(); }

    // 返回 true 表示允许触发回调，调用方必须配对调用 Leave()（或用 Scope）
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

    // RAII：if (EmitGate::Scope s{gate}) { ...回调... }
    class Scope
    {
    public:
        explicit Scope(EmitGate& gate) noexcept : m_gate(gate), m_entered(gate.Enter()) {}
        ~Scope()
        {
            if (m_entered) {
                m_gate.Leave();
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        explicit operator bool() const noexcept { return m_entered; }

    private:
        EmitGate& m_gate;
        bool m_entered;
    };

private:
    // 默认开闸：没有显式启动的路径（如仅 Open 后由宿主直接推帧）保持原有行为
    std::atomic_bool m_open{true};
    std::atomic<int> m_inFlight{0};
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_EMIT_GATE_H
