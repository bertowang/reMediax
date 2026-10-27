// 总线：数据 / 命令 / 事件 三个平面统一在一处
//
//   数据面  节点链式串联（BusNode::LinkNext），节点间用有界队列解耦并按队列深度背压
//           采集 → video_encoder / audio_encoder → muxer，编码后的包也走这条链
//   命令面  Bus::Send / Broadcast 按名字把控制命令投递给节点（Flush / RequestKeyFrame / Abort …）
//           节点通过 BusNode::OnCommand 响应，同步调用、必须非阻塞
//   事件面  节点通过 BusNode::ReportError / ReportNotify 向外发布，由独立分发线程投递给订阅者
//           —— 发布者永远不会被宿主回调阻塞，宿主回调也不会在任何内部锁里被调用
//
// 线程模型
//   - 拓扑（Register / Link）只在构造期做，运行期只读；Get / Send / Dispatch 可并发调用
//   - 事件异步（Publish）由分发线程串行投递；同步（PublishSync）在调用线程内立即投递，
//     两者的订阅者可能在不同线程并发被调用，订阅者必须自己保证线程安全
//   - 订阅者里允许调用任何 Bus / 外层接口（包括 Stop、销毁自身）：
//     分发线程在 Shutdown 时若发现自己就是调用者，会分离线程而不是 join 自己
#ifndef TGCAPTURE_CORE_BUS_H
#define TGCAPTURE_CORE_BUS_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Frame.h"
#include "Logger.h"

namespace TGCapture::core
{

// ------------------------------------------------------------------ 命令面
enum class BusCommandType
{
    Flush,            // 丢弃节点内尚未处理的积压
    RequestKeyFrame,  // 视频编码器：下一帧强制输出关键帧
    Abort,            // 封装器：立即打断正在进行的网络 / 文件 I/O（拆除路径用，不等超时）
};

struct BusCommand
{
    BusCommandType type = BusCommandType::Flush;
    int64_t arg = 0;   // 命令自定义参数，当前命令都用不到
};

// ------------------------------------------------------------------ 事件面
enum class BusEventType
{
    Error,      // 错误：code = 错误码（见 TGCaptureError），message = 描述
    Notify,     // 通知：code = NotifyMsgType，message = 内容，data = 附带数值
    Prepared,   // 生命周期：Prepare 成功
    Started,    // 生命周期：Start 成功
    Stopped,    // 生命周期：Stop 完成，code = 错误码（0 = 正常）
};

struct BusEvent
{
    BusEventType type = BusEventType::Notify;
    std::string source;      // 发布者节点名（"muxer" / "video_encoder" / "capturer" …）
    int32_t code = 0;
    int32_t extra = 0;
    int64_t data = 0;
    std::string message;
    int64_t tsUs = 0;        // 发布时刻（steady_clock 微秒），由 EventHub 填写
};

class EventHub : public std::enable_shared_from_this<EventHub>
{
public:
    using Handler = std::function<void(const BusEvent&)>;

    // 请用 EventHub::Create 创建：分发线程持有 shared_ptr<EventHub>，
    // 这样即使在分发线程内部销毁外层对象，EventHub 自己也能安全收尾
    static std::shared_ptr<EventHub> Create(size_t capacity = 256)
    {
        auto hub = std::shared_ptr<EventHub>(new EventHub(capacity));
        hub->Start();
        return hub;
    }

    ~EventHub() { Shutdown(); }

    EventHub(const EventHub&) = delete;
    EventHub& operator=(const EventHub&) = delete;

    // 订阅：返回 id 供 Unsubscribe。订阅者按订阅顺序依次被调用
    uint64_t Subscribe(Handler handler)
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        auto next = std::make_shared<std::vector<Entry>>(*m_handlers);
        const uint64_t id = ++m_nextId;
        next->push_back(Entry{id, std::move(handler)});
        m_handlers = std::move(next);
        return id;
    }

    void Unsubscribe(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        auto next = std::make_shared<std::vector<Entry>>();
        for (const auto& e : *m_handlers) {
            if (e.id != id) {
                next->push_back(e);
            }
        }
        m_handlers = std::move(next);
    }

    // 异步发布：入队即返回，永不阻塞发布者。
    // 队列满时丢最旧的非 Error 事件（错误不能被通知淹没），全是 Error 才丢最旧的
    void Publish(BusEvent event)
    {
        event.tsUs = NowUs();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                return;
            }
            if (m_queue.size() >= m_capacity) {
                auto victim = m_queue.begin();
                for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
                    if (it->type != BusEventType::Error) {
                        victim = it;
                        break;
                    }
                }
                m_queue.erase(victim);
                m_dropped.fetch_add(1);
            }
            m_queue.push_back(std::move(event));
        }
        m_cv.notify_one();
    }

    // 同步发布：在调用线程内立即投递给全部订阅者（用于必须在调用方线程、且在其解锁之后
    // 触发的生命周期回调）。已 Shutdown 则不投递
    void PublishSync(BusEvent event)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                return;
            }
        }
        event.tsUs = NowUs();
        Dispatch(event);
    }

    // 等到队列清空且没有事件正在被投递。用于 Stop 收尾：保证"先发生的错误先被宿主看到，再看到 Stopped"。
    // 在分发线程内调用会等不到自己，直接返回 true
    bool Drain(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
    {
        if (OnDispatchThread()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_idleCv.wait_for(lock, timeout,
                                 [this] { return m_closed || (m_queue.empty() && !m_inflight); });
    }

    // 幂等。普通线程：停止并 join。分发线程自己调用（订阅者里销毁了外层对象）：分离，线程自行退出
    void Shutdown()
    {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            m_queue.clear();
            thread = std::move(m_thread);
        }
        m_cv.notify_all();
        m_idleCv.notify_all();
        if (!thread.joinable()) {
            return;
        }
        if (OnDispatchThread()) {
            thread.detach();
        } else {
            thread.join();
        }
    }

    [[nodiscard]] bool OnDispatchThread() const noexcept { return t_current == this; }
    [[nodiscard]] uint64_t Dropped() const noexcept { return m_dropped.load(); }
    [[nodiscard]] size_t Pending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    struct Entry
    {
        uint64_t id;
        Handler fn;
    };

    explicit EventHub(size_t capacity) : m_capacity(capacity > 0 ? capacity : 1) {}

    static int64_t NowUs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void Start()
    {
        auto self = shared_from_this();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thread = std::thread([self] { self->Loop(); });
    }

    void Loop()
    {
        t_current = this;
        for (;;) {
            BusEvent event;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_closed || !m_queue.empty(); });
                if (m_closed) {
                    break;   // 关闭即退出：订阅者可能已经指向被销毁的外层对象，不再投递残余事件
                }
                event = std::move(m_queue.front());
                m_queue.pop_front();
                m_inflight = true;
            }
            Dispatch(event);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_inflight = false;
            }
            m_idleCv.notify_all();
        }
        t_current = nullptr;
    }

    void Dispatch(const BusEvent& event)
    {
        std::shared_ptr<const std::vector<Entry>> handlers;
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            handlers = m_handlers;
        }
        for (const auto& entry : *handlers) {
            try {
                entry.fn(event);
            } catch (const std::exception& e) {
                TG_LOGE("Bus: 事件订阅者抛出异常 source=%s code=%d what=%s", event.source.c_str(),
                        event.code, e.what());
            } catch (...) {
                TG_LOGE("Bus: 事件订阅者抛出未知异常 source=%s code=%d", event.source.c_str(),
                        event.code);
            }
        }
    }

    inline static thread_local const EventHub* t_current = nullptr;

    const size_t m_capacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    std::deque<BusEvent> m_queue;
    std::thread m_thread;
    bool m_closed = false;
    bool m_inflight = false;
    std::atomic<uint64_t> m_dropped{0};

    std::mutex m_handlerMutex;
    std::shared_ptr<const std::vector<Entry>> m_handlers =
        std::make_shared<const std::vector<Entry>>();
    uint64_t m_nextId = 0;
};

// ------------------------------------------------------------------ 数据面节点
class BusNode
{
public:
    explicit BusNode(std::string name) : m_name(std::move(name)) {}
    virtual ~BusNode() = default;

    [[nodiscard]] const std::string& Name() const noexcept { return m_name; }

    // 上游提交（入队）。返回 false 表示节点已停止
    virtual bool Submit(FramePtr frame) = 0;

    // 当前积压数量，用于上游背压判断
    [[nodiscard]] virtual size_t Pending() const = 0;

    // 命令：返回 true 表示已处理。实现必须非阻塞（调用方可能是事件分发线程或采集线程）
    virtual bool OnCommand(const BusCommand& /*command*/) { return false; }

    // 拓扑只在构造期设置、运行期只读 —— Forward 为了热路径不加锁不拷贝 shared_ptr
    void LinkNext(std::shared_ptr<BusNode> next) { m_next = std::move(next); }
    [[nodiscard]] std::shared_ptr<BusNode> Next() const noexcept { return m_next; }

    // 由 Bus::Register 调用，之后本节点才能发布事件
    void AttachEvents(std::shared_ptr<EventHub> hub) { m_events = std::move(hub); }

protected:
    // 交给下一节点；链尾返回 true 表示流程结束
    bool Forward(FramePtr frame)
    {
        if (!m_next) {
            return true;
        }
        return m_next->Submit(std::move(frame));
    }

    // 下游积压：链尾为 0
    [[nodiscard]] size_t DownstreamPending() const { return m_next ? m_next->Pending() : 0; }

    void ReportError(int32_t code, std::string message, int32_t extra = 0)
    {
        Report(BusEventType::Error, code, std::move(message), extra, 0);
    }

    void ReportNotify(int32_t type, std::string message, int64_t data = 0)
    {
        Report(BusEventType::Notify, type, std::move(message), 0, data);
    }

private:
    void Report(BusEventType type, int32_t code, std::string message, int32_t extra, int64_t data)
    {
        if (!m_events) {
            return;   // 未挂总线（单测 / 独立使用）：静默
        }
        BusEvent event;
        event.type = type;
        event.source = m_name;
        event.code = code;
        event.extra = extra;
        event.data = data;
        event.message = std::move(message);
        m_events->Publish(std::move(event));
    }

    std::string m_name;
    std::shared_ptr<BusNode> m_next;
    std::shared_ptr<EventHub> m_events;
};

using BusNodePtr = std::shared_ptr<BusNode>;

// ------------------------------------------------------------------ 总线
class Bus
{
public:
    Bus() : m_events(EventHub::Create()) {}
    ~Bus() { m_events->Shutdown(); }

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // ---- 拓扑 ----
    // 注册节点并把事件通道接给它。同名重复注册会替换
    void Register(const std::string& name, BusNodePtr node)
    {
        if (!node) {
            return;
        }
        node->AttachEvents(m_events);
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_nodes[name] = std::move(node);
    }

    // 把 from 的输出接到 to。任一不存在返回 false（拓扑错误应在构造期暴露）
    bool Link(const std::string& from, const std::string& to)
    {
        auto src = Get(from);
        auto dst = Get(to);
        if (!src || !dst || src == dst) {
            return false;
        }
        src->LinkNext(std::move(dst));
        return true;
    }

    BusNodePtr Get(const std::string& name) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_nodes.find(name);
        return it == m_nodes.end() ? nullptr : it->second;
    }

    // ---- 数据面 ----
    // 按名字投递。热路径请缓存 Get() 的结果直接 Submit，避免每帧一次字符串哈希
    bool Dispatch(const std::string& entry, FramePtr frame) const
    {
        auto node = Get(entry);
        return node && node->Submit(std::move(frame));
    }

    [[nodiscard]] size_t TotalPending() const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        size_t total = 0;
        for (const auto& [_, node] : m_nodes) {
            total += node->Pending();
        }
        return total;
    }

    [[nodiscard]] size_t PendingOf(const std::string& name) const
    {
        auto node = Get(name);
        return node ? node->Pending() : 0;
    }

    [[nodiscard]] std::vector<std::string> Dump() const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::vector<std::string> lines;
        for (const auto& [name, node] : m_nodes) {
            lines.push_back(name + " pending=" + std::to_string(node->Pending()));
        }
        return lines;
    }

    // ---- 命令面 ----
    // 投递给指定节点；节点不存在或不认识该命令返回 false
    bool Send(const std::string& target, const BusCommand& command) const
    {
        auto node = Get(target);
        return node && node->OnCommand(command);
    }

    // 投递给全部节点，返回处理了该命令的节点数
    size_t Broadcast(const BusCommand& command) const
    {
        std::vector<BusNodePtr> nodes;
        {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            nodes.reserve(m_nodes.size());
            for (const auto& [_, node] : m_nodes) {
                nodes.push_back(node);
            }
        }
        size_t handled = 0;
        for (const auto& node : nodes) {
            if (node->OnCommand(command)) {
                ++handled;
            }
        }
        return handled;
    }

    // ---- 事件面 ----
    [[nodiscard]] EventHub& Events() noexcept { return *m_events; }
    [[nodiscard]] const std::shared_ptr<EventHub>& EventsPtr() const noexcept { return m_events; }

    uint64_t Subscribe(EventHub::Handler handler) { return m_events->Subscribe(std::move(handler)); }

    // 非节点身份发布（门面自己的错误 / 生命周期）
    void Publish(BusEventType type, std::string source, int32_t code, std::string message,
                 int32_t extra = 0, int64_t data = 0)
    {
        m_events->Publish(MakeEvent(type, std::move(source), code, std::move(message), extra, data));
    }

    void PublishSync(BusEventType type, std::string source, int32_t code, std::string message,
                     int32_t extra = 0, int64_t data = 0)
    {
        m_events->PublishSync(MakeEvent(type, std::move(source), code, std::move(message), extra, data));
    }

private:
    static BusEvent MakeEvent(BusEventType type, std::string source, int32_t code,
                              std::string message, int32_t extra, int64_t data)
    {
        BusEvent event;
        event.type = type;
        event.source = std::move(source);
        event.code = code;
        event.extra = extra;
        event.data = data;
        event.message = std::move(message);
        return event;
    }

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, BusNodePtr> m_nodes;
    std::shared_ptr<EventHub> m_events;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_BUS_H
