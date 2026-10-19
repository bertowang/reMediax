// 线程基类：基于 C++20 std::jthread + stop_token，停止即join，杜绝线程泄漏与野线程
#ifndef TGCAPTURE_CORE_THREAD_H
#define TGCAPTURE_CORE_THREAD_H

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "core/Logger.h"

namespace TGCapture::core
{

class Worker
{
public:
    explicit Worker(std::string name = "worker") : m_name(std::move(name)) {}

    // 析构即停止：保证任何路径下线程都被回收
    virtual ~Worker() { Stop(); }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool Start()
    {
        if (m_running.exchange(true)) {
            return false;  // 已在运行
        }
        // 注意：必须使用 jthread 自己的 stop_token，
        // 外部另建 stop_source 无法停止该线程（会导致 Stop 永久等待）
        m_thread = std::jthread([this](std::stop_token st) {
            OnThreadEnter();
            TG_LOGD("Worker[%s] 线程进入", m_name.c_str());
            Loop(st);
            OnThreadExit();
            m_running.store(false);
        });
        TG_LOGD("Worker[%s] 已启动", m_name.c_str());
        return true;
    }

    void Stop()
    {
        if (!m_running.load()) {
            return;
        }
        RequestStop();
        Join();
    }

    void RequestStop()
    {
        if (m_thread.joinable()) {
            m_thread.request_stop();
        }
        m_wakeup = true;
    }

    void Join()
    {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_running.store(false);
    }

    [[nodiscard]] bool IsRunning() const noexcept { return m_running.load(); }
    [[nodiscard]] bool StopRequested() const noexcept
    {
        return m_thread.joinable() && m_thread.get_stop_token().stop_requested();
    }
    [[nodiscard]] const std::string& Name() const noexcept { return m_name; }

    // 让子类可以打断等待（配合 ConditionVariable / 队列超时）
    void NotifyWakeup() { m_wakeup = true; }
    bool ConsumeWakeup()
    {
        bool expected = true;
        return m_wakeup.compare_exchange_strong(expected, false);
    }

protected:
    // 子类实现：必须周期性检查 st.stop_requested()
    virtual void Loop(std::stop_token st) = 0;
    virtual void OnThreadEnter() {}
    virtual void OnThreadExit() {}

    static void SleepFor(std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }

private:
    std::string m_name;
    std::jthread m_thread;
    std::atomic_bool m_running{false};
    std::atomic_bool m_wakeup{false};
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_THREAD_H
