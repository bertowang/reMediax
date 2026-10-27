// 故障判定原语：把"连续失败多久算故障、恢复多久算恢复"和"多久没数据算断流"收在一处。
//
// 动机：写包失败、编码失败、采集断流原先各写一套 ad-hoc 判定（有的用计数、有的用时长、
// 有的根本没有），导致同一类问题在不同模块的可观测性不一致。这里只提供判定，不做上报，
// 上报码与文案由各模块自己决定。
//
// 线程模型：两个类都是"单写多读"——由所属模块的工作线程调用 OnXxx / Poll，
// 其它线程（统计 / 心跳）只读 Failing / Stalled / 计数，这些读取走原子。
#ifndef TGCAPTURE_CORE_HEALTH_H
#define TGCAPTURE_CORE_HEALTH_H

#include <atomic>
#include <climits>
#include <cstdint>

namespace TGCapture::core
{

// 连续失败追踪：一轮"故障"= 从第一次失败到确认恢复，期间只上报一次。
class FailureTracker
{
public:
    // reportAfterUs  连续失败持续多久才算故障（滤掉偶发抖动，避免刷宿主回调）
    // recoverAfterUs 失败后要连续成功多久才算恢复。
    //                带缓冲的 I/O（AVIO）在断流后紧接着的几次写只是进了用户态缓冲，
    //                必须等缓冲刷过几轮仍然成功，才能确认链路真的好了。
    void Configure(int64_t reportAfterUs, int64_t recoverAfterUs) noexcept
    {
        m_reportAfterUs = reportAfterUs > 0 ? reportAfterUs : 0;
        m_recoverAfterUs = recoverAfterUs > 0 ? recoverAfterUs : 0;
    }

    void Reset() noexcept
    {
        m_failSinceUs.store(kNone);
        m_failures.store(0);
        m_roundFailures = 0;
        m_okSinceUs = kNone;
        m_reported = false;
    }

    // 返回 true 表示"本轮故障刚刚达到上报门限"，调用方据此上报一次
    bool OnFailure(int64_t nowUs) noexcept
    {
        m_failures.fetch_add(1);
        ++m_roundFailures;
        m_okSinceUs = kNone;   // 观察期作废：恢复必须从"最后一次失败之后"重新连续成功
        if (m_failSinceUs.load() == kNone) {
            m_failSinceUs.store(nowUs);
        }
        if (m_reported) {
            return false;
        }
        if (nowUs - m_failSinceUs.load() >= m_reportAfterUs) {
            m_reported = true;
            return true;
        }
        return false;
    }

    // 返回 true 表示"本轮故障确认结束"（只有上报过的故障才需要通告恢复）
    bool OnSuccess(int64_t nowUs) noexcept
    {
        if (m_failSinceUs.load() == kNone) {
            return false;   // 本来就是正常态
        }
        if (m_okSinceUs == kNone) {
            // 观察期从"第一次成功"而不是"最后一次失败"起算：
            // 两次调用之间没有数据流动，那段空档不能当成功来算
            m_okSinceUs = nowUs;
        }
        if (nowUs - m_okSinceUs < m_recoverAfterUs) {
            return false;   // 还在观察期：缓冲写入的"成功"不算恢复
        }
        const bool wasReported = m_reported;
        m_failSinceUs.store(kNone);
        m_roundFailures = 0;
        m_okSinceUs = kNone;
        m_reported = false;
        return wasReported;
    }

    [[nodiscard]] bool Failing() const noexcept { return m_failSinceUs.load() != kNone; }
    [[nodiscard]] bool Reported() const noexcept { return m_reported; }
    [[nodiscard]] int64_t FailingUs(int64_t nowUs) const noexcept
    {
        const int64_t since = m_failSinceUs.load();
        return since == kNone ? 0 : nowUs - since;
    }
    [[nodiscard]] uint64_t Failures() const noexcept { return m_failures.load(); }
    [[nodiscard]] uint64_t RoundFailures() const noexcept { return m_roundFailures; }

private:
    // "无"哨兵取 INT64_MIN 而不是 0：单调钟在进程刚起来时可能就是 0，
    // 用 0 当哨兵就得把真实时间戳挪成 1，所有时长都会差 1us
    static constexpr int64_t kNone = INT64_MIN;

    int64_t m_reportAfterUs = 0;
    int64_t m_recoverAfterUs = 0;
    std::atomic<int64_t> m_failSinceUs{kNone};   // 本轮失败起点，kNone = 当前正常
    std::atomic<uint64_t> m_failures{0};         // 累计失败次数（跨轮）
    uint64_t m_roundFailures = 0;                // 本轮失败次数
    int64_t m_okSinceUs = kNone;                 // 本轮第一次成功的时刻（恢复观察期起点）
    bool m_reported = false;
};

// 断流看护：数据线程 OnActivity 打点，看护线程周期 Poll 判定。
// 用来覆盖"设备丢失 / 权限被撤销 / 采集重建失败"这类**没有失败返回值**的故障 ——
// 它们的共同表现就是"不再有帧"，在内核侧统一探测比让每个平台各自上报更可靠。
class StallWatch
{
public:
    enum class Transition
    {
        None,
        Stalled,     // 刚刚判定为断流
        Recovered,   // 断流后又有数据了
    };

    // timeoutUs <= 0 表示关闭看护。
    // SetAVTimeout 允许运行期调用，所以这里是宿主线程写、看护线程读 —— 必须原子。
    void Configure(int64_t timeoutUs) noexcept
    {
        m_timeoutUs.store(timeoutUs > 0 ? timeoutUs : 0, std::memory_order_relaxed);
    }

    // 开播时调用：把基准置为"现在"，否则会把 Prepare 到 Start 之间的空档算成断流
    void Reset(int64_t nowUs) noexcept
    {
        m_lastActivityUs.store(nowUs);
        m_stalled.store(false);
        m_stalls.store(0);
    }

    void OnActivity(int64_t nowUs) noexcept
    {
        // 单调推进：乱序调用（多路音频）不会把基准拨回过去
        int64_t cur = m_lastActivityUs.load(std::memory_order_relaxed);
        while (nowUs > cur) {
            if (m_lastActivityUs.compare_exchange_weak(cur, nowUs, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    Transition Poll(int64_t nowUs) noexcept
    {
        // 只读一次：Configure 可能在本次判定中途被宿主线程改写，
        // 读两次会出现"用新阈值判 idle、用旧阈值算时长"的不一致
        const int64_t timeoutUs = m_timeoutUs.load(std::memory_order_relaxed);
        if (timeoutUs == 0) {
            return Transition::None;
        }
        const bool idle = (nowUs - m_lastActivityUs.load()) > timeoutUs;
        if (idle && !m_stalled.load()) {
            m_stalled.store(true);
            m_stalls.fetch_add(1);
            return Transition::Stalled;
        }
        if (!idle && m_stalled.load()) {
            m_stalled.store(false);
            return Transition::Recovered;
        }
        return Transition::None;
    }

    [[nodiscard]] int64_t IdleUs(int64_t nowUs) const noexcept
    {
        const int64_t idle = nowUs - m_lastActivityUs.load();
        return idle > 0 ? idle : 0;
    }
    [[nodiscard]] bool Stalled() const noexcept { return m_stalled.load(); }
    [[nodiscard]] uint64_t Stalls() const noexcept { return m_stalls.load(); }
    [[nodiscard]] int64_t TimeoutUs() const noexcept
    {
        return m_timeoutUs.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> m_timeoutUs{0};
    std::atomic<int64_t> m_lastActivityUs{0};
    std::atomic_bool m_stalled{false};
    std::atomic<uint64_t> m_stalls{0};
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_HEALTH_H
