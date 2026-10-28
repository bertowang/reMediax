// 流量计：同一份写出数据同时给出"实时"和"累计"两种口径（两者回答的问题不同，缺一不可）
//
//   实时  Bps()        固定窗口（默认 1s）翻转计算的速率，用来看"链路此刻是否健康"。
//                      超过 2 个窗口没有新数据（写出停滞 / 断流）即归 0 ——
//                      否则会一直挂着断流前的值，恰好在最需要它的时候给出错误结论。
//   累计  TotalBytes() 自第一包起的总字节数，用来核对产出量（≈ 文件大小 / 上行流量）。
//         AverageBps() 总字节 / 自第一包起的时长；停滞期间时长照算，平均值随之下降，如实反映"实际交付"。
//
// 线程模型：Reset / Add 只由写出线程调用（单写者）；读接口任意线程可调，只读原子量。
// 时间由调用方传入（单调时钟微秒），便于单测精确控制。
#ifndef TGCAPTURE_CORE_TRAFFIC_METER_H
#define TGCAPTURE_CORE_TRAFFIC_METER_H

#include <atomic>
#include <cstdint>
#include <limits>

namespace TGCapture::core
{

class TrafficMeter
{
public:
    explicit TrafficMeter(int64_t windowUs = 1000000) noexcept : m_windowUs(windowUs > 0 ? windowUs : 1000000) {}

    // 重开时调用；必须在写出线程启动前 / 停止后调用（与 Add 不并发）
    void Reset() noexcept
    {
        m_total.store(0);
        m_bps.store(0);
        m_publishedUs.store(kNever);
        m_firstUs.store(kNever);
        m_windowStartUs = 0;
        m_windowBytes = 0;
    }

    // 写出线程：记一笔已成功写出的字节
    void Add(uint64_t bytes, int64_t nowUs) noexcept
    {
        if (m_firstUs.load(std::memory_order_relaxed) == kNever) {
            m_firstUs.store(nowUs);
            m_windowStartUs = nowUs;
        }
        // 先结算上一个窗口 [start, now)，再把本笔记入新窗口：边界上的包不会被算进两边
        const int64_t elapsed = nowUs - m_windowStartUs;
        if (elapsed >= m_windowUs) {
            m_bps.store(static_cast<uint64_t>(static_cast<double>(m_windowBytes) * 8.0 * 1e6 /
                                              static_cast<double>(elapsed)));
            m_publishedUs.store(nowUs);
            m_windowStartUs = nowUs;
            m_windowBytes = 0;
        }
        m_windowBytes += bytes;
        m_total.fetch_add(bytes);
    }

    // 实时速率（bit/s）。第一个窗口结束前为 0；停滞超过 2 个窗口归 0
    [[nodiscard]] uint64_t Bps(int64_t nowUs) const noexcept
    {
        const int64_t published = m_publishedUs.load();
        if (published == kNever || nowUs - published > 2 * m_windowUs) {
            return 0;
        }
        return m_bps.load();
    }

    [[nodiscard]] uint64_t TotalBytes() const noexcept { return m_total.load(); }

    // 自第一包起的时长（微秒）；还没有数据时为 0
    [[nodiscard]] int64_t ElapsedUs(int64_t nowUs) const noexcept
    {
        const int64_t first = m_firstUs.load();
        return (first == kNever || nowUs <= first) ? 0 : nowUs - first;
    }

    // 平均速率（bit/s）= 总字节 / 自第一包起的时长
    [[nodiscard]] uint64_t AverageBps(int64_t nowUs) const noexcept
    {
        const int64_t elapsed = ElapsedUs(nowUs);
        if (elapsed <= 0) {
            return 0;
        }
        return static_cast<uint64_t>(static_cast<double>(TotalBytes()) * 8.0 * 1e6 / static_cast<double>(elapsed));
    }

private:
    static constexpr int64_t kNever = std::numeric_limits<int64_t>::min();

    const int64_t m_windowUs;
    std::atomic<uint64_t> m_total{0};
    std::atomic<uint64_t> m_bps{0};
    std::atomic<int64_t> m_publishedUs{kNever};   // 最近一次结算窗口的时刻
    std::atomic<int64_t> m_firstUs{kNever};       // 第一笔数据的时刻

    // ---- 仅写出线程 ----
    int64_t m_windowStartUs = 0;
    uint64_t m_windowBytes = 0;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_TRAFFIC_METER_H
