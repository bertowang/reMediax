// 有界队列：连接总线各节点，提供背压（满则按策略丢弃最旧帧，保证直播低延迟）
#ifndef TGCAPTURE_CORE_QUEUE_H
#define TGCAPTURE_CORE_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace TGCapture::core
{

enum class DropPolicy
{
    Block,        // 阻塞等待（用于音频等关键数据）
    DropOldest,   // 丢最旧（视频默认：宁可掉帧也不延迟）
    DropNewest,   // 丢最新
};

template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(size_t capacity, DropPolicy policy = DropPolicy::DropOldest)
        : m_capacity(capacity), m_policy(policy)
    {
    }

    // 返回 false 表示队列已关闭
    bool Push(T item)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_closed) {
                return false;
            }
            if (m_queue.size() >= m_capacity) {
                if (m_policy == DropPolicy::DropOldest) {
                    m_queue.pop_front();
                    ++m_dropped;
                } else if (m_policy == DropPolicy::DropNewest) {
                    ++m_dropped;
                    return true;
                } else {
                    m_notFull.wait(lock, [this] { return m_closed || m_queue.size() < m_capacity; });
                    if (m_closed) {
                        return false;
                    }
                }
            }
            m_queue.push_back(std::move(item));
        }
        m_notEmpty.notify_one();
        return true;
    }

    // 满时丢最旧，但跳过 keep 判为真的元素（典型用途：保护视频关键帧）。
    // 直接用 DropOldest 会在下游拥塞时把 IDR 丢掉，之后整个 GOP 都解不出来（花屏）。
    template <typename Keep>
    bool PushKeepProtected(T item, Keep keep)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_closed) {
                return false;
            }
            if (m_queue.size() >= m_capacity) {
                bool dropped = false;
                for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
                    if (!keep(*it)) {
                        m_queue.erase(it);
                        ++m_dropped;
                        dropped = true;
                        break;
                    }
                }
                if (!dropped) {
                    // 队列里全是受保护元素：也只能丢最旧的，否则会永久塞满不再出包
                    m_queue.pop_front();
                    ++m_dropped;
                }
            }
            m_queue.push_back(std::move(item));
        }
        m_notEmpty.notify_one();
        return true;
    }

    // 阻塞弹出；返回 nullopt 表示已关闭且无数据
    std::optional<T> Pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return m_closed || !m_queue.empty(); });
        if (m_queue.empty()) {
            return std::nullopt;
        }
        T item = std::move(m_queue.front());
        m_queue.pop_front();
        lock.unlock();
        m_notFull.notify_one();
        return item;
    }

    // 带超时弹出（便于响应 stop_token）
    std::optional<T> TryPop(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_notEmpty.wait_for(lock, timeout, [this] { return m_closed || !m_queue.empty(); })) {
            return std::nullopt;
        }
        if (m_queue.empty()) {
            return std::nullopt;
        }
        T item = std::move(m_queue.front());
        m_queue.pop_front();
        lock.unlock();
        m_notFull.notify_one();
        return item;
    }

    void Close()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    // 重新开放：Close() 是"永久关闭"语义（用来唤醒阻塞的消费者），
    // 所以 Stop → Prepare → Start 重开一路流时必须显式 Reopen，
    // 否则第二轮的 Push 会全部返回 false，输出文件只剩一个头（实测 389 字节、0 个包）。
    void Reopen()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.clear();     // 清掉上一轮残留，避免跨轮数据（GPU 模式还会滞留纹理 owner）
            m_closed = false;
            m_dropped = 0;
        }
        m_notFull.notify_all();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }

    [[nodiscard]] size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    [[nodiscard]] bool Empty() const { return Size() == 0; }
    [[nodiscard]] size_t Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] uint64_t DroppedCount() const noexcept { return m_dropped; }

private:
    size_t m_capacity;
    DropPolicy m_policy;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<T> m_queue;
    bool m_closed = false;
    uint64_t m_dropped = 0;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_QUEUE_H
