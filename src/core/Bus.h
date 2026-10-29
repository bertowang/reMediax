// 总线：数据 / 命令 / 事件 三个平面统一在一处
//
//   数据面  节点带输入 / 输出端口，Bus::Link 把 "from:outPort" 接到 "to:inPort"，一个输出端口可以扇出多路。
//           每条连线有三种投递方式（LinkMode）：
//             Direct  生产者线程里同步调用下游 Submit（下游自己有队列时用它，零额外线程）
//             Queued  连线自带有界队列 + 投递线程：解耦慢下游；满了按策略丢最旧的数据帧 / 限时阻塞，
//                     控制帧（FormatChanged / Flush / EOS）永不丢弃
//             Latest  只保留最新一帧（预览类下游：慢了就跳帧，永远看最新画面）
//           数据通道里可以夹带控制帧（Frame::control），与数据严格保序。
//   命令面  Bus::Send / Broadcast 按名字把控制命令投递给节点（Flush / RequestKeyFrame / Abort / SetBitrate …），
//           命令可携带类型化参数（BusCommand::payload）。节点通过 OnCommand 响应，同步调用、必须非阻塞
//   事件面  节点通过 BusNode::ReportError / ReportNotify 向外发布，由独立分发线程投递给订阅者
//           —— 发布者永远不会被宿主回调阻塞，宿主回调也不会在任何内部锁里被调用
//   上下文  BusContext：按类型注入共享服务（音频时钟、同步、统计、预设…），它们不是节点、不走数据面
//
// 运行期拓扑
//   拓扑是不可变快照（RCU）：读路径（Emit / Get / Dump）只拿快照，不与修改互斥。
//   修改走事务：Begin() → Register / Unregister / Link / Unlink → Commit()，要么全部生效要么全部不生效。
//   Commit 保证：
//     - 新增连线：先回调下游 OnInputAttached，再开始投递（下游可以在回调里准备好该输入的状态）
//     - 删除连线：等上游正在进行的投递全部结束（宽限期）、连线队列线程退出之后，
//                 才回调下游 OnInputDetached —— 回调之后该输入不会再收到任何帧
//     - 拒绝自环 / 成环 / 端口越界 / 重复连线 / 不存在的节点
//   OnInputAttached / Detached 回调里不能再提交事务（会被拒绝并返回 false，而不是死锁）。
//
// 线程模型
//   - Get / Send / Dispatch / Emit / Dump 可任意线程并发调用；Commit 之间互斥
//   - 事件异步（Publish）由分发线程串行投递；同步（PublishSync）在调用线程内立即投递，
//     两者的订阅者可能在不同线程并发被调用，订阅者必须自己保证线程安全
//   - 订阅者里允许调用任何 Bus / 外层接口（包括 Stop、销毁自身）：
//     分发线程在 Shutdown 时若发现自己就是调用者，会分离线程而不是 join 自己
#ifndef TGCAPTURE_CORE_BUS_H
#define TGCAPTURE_CORE_BUS_H

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "EmitGate.h"
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
    SetBitrate,       // 编码器：arg = 新目标码率（bps），不重开编码器
    SetParam,         // 通用参数：key = 参数名，payload = 类型化参数（节点自行约定类型）
};

struct BusCommand
{
    BusCommandType type = BusCommandType::Flush;
    int64_t arg = 0;        // 简单整型参数（SetBitrate 的码率等）
    std::string key;        // SetParam 的参数名 / 子对象 id
    std::any payload;       // 类型化参数，用 Param<T>() 取

    template <typename T>
    [[nodiscard]] const T* Param() const noexcept
    {
        return std::any_cast<T>(&payload);
    }
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

// ------------------------------------------------------------------ 上下文（共享服务注入）
// 不是节点、不走数据面的共享对象（音频时钟、A/V 同步、统计、预设表…）按类型注册，节点按类型取。
// 同一类型只存一个实例；Provide 覆盖旧实例。线程安全
class BusContext
{
public:
    template <typename T>
    void Provide(std::shared_ptr<T> service)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (service) {
            m_services[std::type_index(typeid(T))] = std::move(service);
        } else {
            m_services.erase(std::type_index(typeid(T)));
        }
    }

    template <typename T>
    [[nodiscard]] std::shared_ptr<T> Get() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(std::type_index(typeid(T)));
        return it == m_services.end() ? nullptr : std::static_pointer_cast<T>(it->second);
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_services;
};

// ------------------------------------------------------------------ 连线
enum class LinkMode : uint8_t
{
    Direct,   // 生产者线程同步调用下游 Submit
    Queued,   // 有界队列 + 投递线程
    Latest,   // 只保留最新一帧 + 投递线程
};

enum class QueueFullPolicy : uint8_t
{
    DropOldest,   // 丢最旧的数据帧（控制帧不丢）
    Block,        // 限时阻塞生产者，超时仍满则丢最旧的数据帧
};

struct LinkOptions
{
    LinkMode mode = LinkMode::Direct;
    size_t capacity = 8;                               // Queued 的队列长度
    QueueFullPolicy onFull = QueueFullPolicy::DropOldest;
    std::chrono::milliseconds blockTimeout{50};        // Block 策略的最长等待

    static LinkOptions Direct() { return {}; }
    static LinkOptions Queued(size_t capacity = 8,
                              QueueFullPolicy policy = QueueFullPolicy::DropOldest)
    {
        LinkOptions o;
        o.mode = LinkMode::Queued;
        o.capacity = capacity > 0 ? capacity : 1;
        o.onFull = policy;
        return o;
    }
    static LinkOptions Latest()
    {
        LinkOptions o;
        o.mode = LinkMode::Latest;
        o.capacity = 1;
        return o;
    }
};

inline const char* LinkModeName(LinkMode mode) noexcept
{
    switch (mode) {
        case LinkMode::Direct: return "direct";
        case LinkMode::Queued: return "queued";
        case LinkMode::Latest: return "latest";
    }
    return "?";
}

// 端口号：输入端口数为 kDynamicPorts 表示"任意多个输入"（混音器 / 合成器），
// Link 的 inPort 给 kAnyPort 表示"由总线分配一个空闲端口"
inline constexpr size_t kDynamicPorts = std::numeric_limits<size_t>::max();
inline constexpr size_t kAnyPort = std::numeric_limits<size_t>::max();

struct LinkStats
{
    std::atomic<uint64_t> delivered{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> rejected{0};   // 下游 Submit 返回 false
};

class BusNode;

// Queued / Latest 连线的队列与投递线程
class LinkChannel : public std::enable_shared_from_this<LinkChannel>
{
public:
    LinkChannel(std::shared_ptr<BusNode> dst, size_t dstPort, LinkOptions options,
                std::shared_ptr<LinkStats> stats)
        : m_dst(std::move(dst)), m_dstPort(dstPort), m_options(options), m_stats(std::move(stats))
    {
    }

    ~LinkChannel() { Stop(); }

    LinkChannel(const LinkChannel&) = delete;
    LinkChannel& operator=(const LinkChannel&) = delete;

    void Start()
    {
        auto self = shared_from_this();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thread = std::thread([self] { self->Loop(); });
    }

    // 入队。已停止返回 false
    bool Push(FramePtr frame)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_closed) {
            return false;
        }
        if (m_options.mode == LinkMode::Latest) {
            // 只留最新：丢掉队里所有数据帧，控制帧保留（保序）
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                if (!(*it)->IsControl()) {
                    it = m_queue.erase(it);
                    m_stats->dropped.fetch_add(1);
                } else {
                    ++it;
                }
            }
        } else if (!frame->IsControl() && CountData() >= m_options.capacity) {
            if (m_options.onFull == QueueFullPolicy::Block) {
                m_spaceCv.wait_for(lock, m_options.blockTimeout,
                                   [this] { return m_closed || CountData() < m_options.capacity; });
                if (m_closed) {
                    return false;
                }
            }
            if (CountData() >= m_options.capacity) {
                DropOldestData();
            }
        }
        m_queue.push_back(std::move(frame));
        lock.unlock();
        m_cv.notify_one();
        return true;
    }

    [[nodiscard]] size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size() + (m_delivering ? 1 : 0);
    }

    // 停止投递线程（幂等）。队里剩余的帧直接丢弃。
    // 在投递线程自己内部调用（下游回调里拆了这条线）时分离线程，不 join 自己
    void Stop()
    {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            m_queue.clear();
            thread = std::move(m_thread);
        }
        m_cv.notify_all();
        m_spaceCv.notify_all();
        if (!thread.joinable()) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }

private:
    size_t CountData() const
    {
        return static_cast<size_t>(std::count_if(m_queue.begin(), m_queue.end(),
                                                 [](const FramePtr& f) { return !f->IsControl(); }));
    }

    void DropOldestData()
    {
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (!(*it)->IsControl()) {
                m_queue.erase(it);
                m_stats->dropped.fetch_add(1);
                return;
            }
        }
    }

    void Loop();   // 依赖 BusNode 的完整定义，见下方

    std::shared_ptr<BusNode> m_dst;
    const size_t m_dstPort;
    const LinkOptions m_options;
    std::shared_ptr<LinkStats> m_stats;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_spaceCv;
    std::deque<FramePtr> m_queue;
    std::thread m_thread;
    bool m_closed = false;
    bool m_delivering = false;
};

// 一个输出端口上的一条出边（节点内部的路由表项）
struct BusRoute
{
    std::shared_ptr<BusNode> dst;
    size_t dstPort = 0;
    std::shared_ptr<LinkChannel> channel;   // Direct 为空
    std::shared_ptr<LinkStats> stats;
};
using BusRouteTable = std::vector<std::vector<BusRoute>>;   // [输出端口] -> 出边列表

// ------------------------------------------------------------------ 节点生命周期
// 分组启停：三组可以独立启停 ——
//   preview  视频采集与合成（vsrc.* / video_compositor）：开播前预览即运行，停播后若预览仍开着则继续运行
//   capture  音频采集与混音（asrc.* / audio_mixer）：随开播启停（打开格式依赖编码参数，Prepare 才定）
//   output   同步 / 编码 / 封装：随开播启停
// 开播前预览 = 只起 preview 组；开播 = 再起 output + capture 组；停播回到预览 = 只停 capture + output 组。
enum class NodeGroup : uint8_t
{
    None,      // 不参与分组启停（由持有者自行管理）
    Capture,
    Output,
    Preview,
};

enum class NodeLifecycle : uint8_t
{
    Created,
    Configured,
    Running,
    Stopped,
    Closed,
};

inline const char* NodeGroupName(NodeGroup g) noexcept
{
    switch (g) {
        case NodeGroup::None: return "none";
        case NodeGroup::Capture: return "capture";
        case NodeGroup::Output: return "output";
        case NodeGroup::Preview: return "preview";
    }
    return "?";
}

inline const char* NodeLifecycleName(NodeLifecycle s) noexcept
{
    switch (s) {
        case NodeLifecycle::Created: return "created";
        case NodeLifecycle::Configured: return "configured";
        case NodeLifecycle::Running: return "running";
        case NodeLifecycle::Stopped: return "stopped";
        case NodeLifecycle::Closed: return "closed";
    }
    return "?";
}

// ------------------------------------------------------------------ 数据面节点
class BusNode
{
public:
    explicit BusNode(std::string name) : m_name(std::move(name)) {}
    virtual ~BusNode() = default;

    BusNode(const BusNode&) = delete;
    BusNode& operator=(const BusNode&) = delete;

    [[nodiscard]] const std::string& Name() const noexcept { return m_name; }

    // ---- 端口 ----
    // 输入端口数；kDynamicPorts = 任意多个（输入随连线增删，见 OnInputAttached / Detached）
    [[nodiscard]] virtual size_t InputPorts() const { return 1; }
    [[nodiscard]] virtual size_t OutputPorts() const { return 1; }

    // 上游提交（入队）。返回 false 表示节点已停止。单输入节点只需实现它
    virtual bool Submit(FramePtr frame) = 0;

    // 多输入节点按端口区分来源；默认转给 Submit
    virtual bool SubmitInput(size_t port, FramePtr frame)
    {
        (void)port;
        return Submit(std::move(frame));
    }

    // 当前积压数量，用于上游背压判断
    [[nodiscard]] virtual size_t Pending() const = 0;

    // 命令：返回 true 表示已处理。实现必须非阻塞（调用方可能是事件分发线程或采集线程）
    virtual bool OnCommand(const BusCommand& /*command*/) { return false; }

    // 运行期拓扑变化：新输入接入（在开始投递之前回调）/ 输入断开（在最后一帧投递完之后回调）。
    // 回调在提交事务的线程里，不能再提交事务
    virtual void OnInputAttached(size_t /*port*/, const std::string& /*from*/) {}
    virtual void OnInputDetached(size_t /*port*/, const std::string& /*from*/) {}

    // Dump 时附加的节点状态（一行，可空）
    [[nodiscard]] virtual std::string DumpState() const { return {}; }

    // ---- 生命周期（由 Bus 的分组启停按拓扑顺序调用；不分组的节点由持有者自行调用）----
    // Configure：上游先于下游（编码器先于封装器）；Start：下游先于上游（先有人收再开始产）；
    // Stop：上游先于下游（先断源，下游 drain 完积压）；Close：下游先于上游。
    [[nodiscard]] virtual NodeGroup Group() const { return NodeGroup::None; }
    // 未激活的节点在分组启动时被跳过（如纯视频模式下没有打开的音频编码器）
    [[nodiscard]] virtual bool IsActive() const { return true; }
    // 受控输出：Stop 时先关输出闸、Stop 后等在飞的 Emit 退出（采集类节点用，回调线程不归自己管）。
    // 编码器 / 封装器不能开：它们在 Stop 里要把积压 drain 给下游
    [[nodiscard]] virtual bool GatedOutput() const { return false; }

    virtual int NodeConfigure() { return 0; }
    virtual bool NodeStart() { return true; }
    virtual void NodeStop() {}
    virtual void NodeClose() {}

    [[nodiscard]] NodeLifecycle Lifecycle() const noexcept { return m_lifecycle.load(); }

    // 单节点的完整动作（含输出闸与状态），Bus 的分组启停逐个调用它们；也可单独调用
    int Configure()
    {
        const int ret = NodeConfigure();
        if (ret >= 0) {
            m_lifecycle.store(NodeLifecycle::Configured);
        }
        return ret;
    }

    bool StartNode()
    {
        m_outGate.Open();
        if (!NodeStart()) {
            return false;
        }
        m_lifecycle.store(NodeLifecycle::Running);
        return true;
    }

    void StopNode()
    {
        const bool gated = GatedOutput();
        if (gated) {
            m_outGate.Close();
        }
        NodeStop();
        if (gated) {
            m_outGate.WaitDrained();
        }
        if (m_lifecycle.load() == NodeLifecycle::Running) {
            m_lifecycle.store(NodeLifecycle::Stopped);
        }
    }

    void CloseNode()
    {
        NodeClose();
        m_lifecycle.store(NodeLifecycle::Closed);
    }

    // 独立使用（不经 Bus）时把输出端口 0 直连到 next。挂到 Bus 上后拓扑由 Bus 管理
    void LinkNext(std::shared_ptr<BusNode> next)
    {
        auto table = std::make_shared<BusRouteTable>(1);
        if (next) {
            (*table)[0].push_back(BusRoute{std::move(next), 0, nullptr, std::make_shared<LinkStats>()});
        }
        SetRoutes(std::move(table));
    }

    // 输出端口 0 的第一个下游（兼容旧接口）
    [[nodiscard]] std::shared_ptr<BusNode> Next() const
    {
        auto table = Routes();
        if (!table || table->empty() || (*table)[0].empty()) {
            return nullptr;
        }
        return (*table)[0][0].dst;
    }

    // 由 Bus::Register 调用，之后本节点才能发布事件 / 取上下文
    void AttachEvents(std::shared_ptr<EventHub> hub) { m_events = std::move(hub); }
    void AttachContext(std::shared_ptr<BusContext> ctx) { m_context = std::move(ctx); }

protected:
    // 交给输出端口 0 的全部下游；没有下游返回 true（链尾）
    bool Forward(FramePtr frame) { return Emit(0, std::move(frame)); }

    // 交给指定输出端口的全部下游（扇出共享同一帧，下游不得修改帧内容）。
    // 全部下游都接受返回 true；没有下游返回 true
    bool Emit(size_t port, FramePtr frame)
    {
        if (!frame) {
            return false;
        }
        EmitGate::Scope gate(m_outGate);
        if (!gate) {
            return false;   // 受控输出已关闸（节点停止中）
        }
        EmitGuard guard(*this);
        auto table = Routes();
        if (!table || port >= table->size()) {
            return true;
        }
        const auto& routes = (*table)[port];
        bool ok = true;
        for (size_t i = 0; i < routes.size(); ++i) {
            const auto& r = routes[i];
            FramePtr f = (i + 1 == routes.size()) ? std::move(frame) : frame;
            bool accepted = false;
            if (r.channel) {
                accepted = r.channel->Push(std::move(f));
            } else {
                accepted = r.dst->SubmitInput(r.dstPort, std::move(f));
                if (accepted) {
                    r.stats->delivered.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (!accepted) {
                r.stats->rejected.fetch_add(1, std::memory_order_relaxed);
                ok = false;
            }
        }
        return ok;
    }

    // 下游积压（连线队列 + 下游节点自身积压，多路取最大）；没有下游为 0
    [[nodiscard]] size_t DownstreamPending(size_t port = 0) const
    {
        auto table = Routes();
        if (!table || port >= table->size()) {
            return 0;
        }
        size_t worst = 0;
        for (const auto& r : (*table)[port]) {
            const size_t p = (r.channel ? r.channel->Size() : 0) + r.dst->Pending();
            worst = std::max(worst, p);
        }
        return worst;
    }

    [[nodiscard]] bool HasDownstream(size_t port = 0) const
    {
        auto table = Routes();
        return table && port < table->size() && !(*table)[port].empty();
    }

    [[nodiscard]] std::shared_ptr<BusContext> Context() const { return m_context; }

    void ReportError(int32_t code, std::string message, int32_t extra = 0)
    {
        Report(BusEventType::Error, code, std::move(message), extra, 0);
    }

    void ReportNotify(int32_t type, std::string message, int64_t data = 0)
    {
        Report(BusEventType::Notify, type, std::move(message), 0, data);
    }

private:
    friend class Bus;

    struct EmitGuard
    {
        explicit EmitGuard(BusNode& n) : node(n)
        {
            node.m_emitting.fetch_add(1, std::memory_order_acq_rel);
            ++t_emitDepth;
        }
        ~EmitGuard()
        {
            --t_emitDepth;
            node.m_emitting.fetch_sub(1, std::memory_order_acq_rel);
        }
        BusNode& node;
    };

    [[nodiscard]] std::shared_ptr<const BusRouteTable> Routes() const
    {
        std::lock_guard<std::mutex> lock(m_routeMutex);
        return m_routes;
    }

    void SetRoutes(std::shared_ptr<const BusRouteTable> table)
    {
        std::lock_guard<std::mutex> lock(m_routeMutex);
        m_routes = std::move(table);
    }

    // 宽限期：等本节点所有进行中的 Emit 结束（它们可能还拿着旧路由表）。
    // 当前线程本身在某个 Emit 调用栈里时不等（等自己会死锁），返回 false
    bool WaitQuiescent(std::chrono::milliseconds timeout) const
    {
        if (t_emitDepth > 0) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (m_emitting.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                TG_LOGW("Bus: 节点 %s 的投递超过 %lld ms 未结束，放弃等待宽限期", m_name.c_str(),
                        static_cast<long long>(timeout.count()));
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

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

    inline static thread_local int t_emitDepth = 0;

    std::string m_name;
    mutable std::mutex m_routeMutex;
    std::shared_ptr<const BusRouteTable> m_routes;
    std::atomic<int> m_emitting{0};
    EmitGate m_outGate;
    std::atomic<NodeLifecycle> m_lifecycle{NodeLifecycle::Created};
    std::shared_ptr<EventHub> m_events;
    std::shared_ptr<BusContext> m_context;
};

using BusNodePtr = std::shared_ptr<BusNode>;

inline void LinkChannel::Loop()
{
    for (;;) {
        FramePtr frame;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_closed || !m_queue.empty(); });
            if (m_closed) {
                break;
            }
            frame = std::move(m_queue.front());
            m_queue.pop_front();
            m_delivering = true;
        }
        m_spaceCv.notify_one();
        const bool accepted = m_dst->SubmitInput(m_dstPort, std::move(frame));
        (accepted ? m_stats->delivered : m_stats->rejected).fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_delivering = false;
    }
}

// ------------------------------------------------------------------ 总线
class Bus
{
public:
    Bus() : m_events(EventHub::Create()), m_context(std::make_shared<BusContext>()) {}

    ~Bus()
    {
        std::shared_ptr<const Topology> topo;
        {
            std::lock_guard<std::mutex> commit(m_commitMutex);
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            topo = std::move(m_topo);
            m_topo = std::make_shared<const Topology>();
        }
        // 拆掉总线管理的路由（节点可能比总线活得久），再停掉连线线程
        for (const auto& [_, node] : topo->nodes) {
            node->SetRoutes(nullptr);
        }
        for (const auto& link : topo->links) {
            if (link.channel) {
                link.channel->Stop();
            }
        }
        m_events->Shutdown();
    }

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // ---- 拓扑事务 ----
    class Transaction
    {
    public:
        Transaction& Register(const std::string& name, BusNodePtr node)
        {
            m_ops.push_back(Op{OpType::Register, name, 0, {}, 0, std::move(node), {}});
            return *this;
        }
        Transaction& Unregister(const std::string& name)
        {
            m_ops.push_back(Op{OpType::Unregister, name, 0, {}, 0, nullptr, {}});
            return *this;
        }
        Transaction& Link(const std::string& from, size_t outPort, const std::string& to,
                          size_t inPort = 0, LinkOptions options = {})
        {
            m_ops.push_back(Op{OpType::Link, from, outPort, to, inPort, nullptr, options});
            return *this;
        }
        Transaction& Link(const std::string& from, const std::string& to, LinkOptions options = {})
        {
            return Link(from, 0, to, 0, options);
        }
        // 拆掉 from -> to 的连线；端口给 kAnyPort 表示该方向上所有端口
        Transaction& Unlink(const std::string& from, const std::string& to,
                            size_t outPort = kAnyPort, size_t inPort = kAnyPort)
        {
            m_ops.push_back(Op{OpType::Unlink, from, outPort, to, inPort, nullptr, {}});
            return *this;
        }

        // 全部生效返回 true；任何一步不合法则全部不生效，原因见 Error()
        bool Commit()
        {
            if (m_committed) {
                m_error = "事务已提交过";
                return false;
            }
            m_committed = true;
            return m_bus.CommitOps(m_ops, m_error);
        }

        [[nodiscard]] const std::string& Error() const noexcept { return m_error; }

    private:
        friend class Bus;
        explicit Transaction(Bus& bus) : m_bus(bus) {}

        enum class OpType { Register, Unregister, Link, Unlink };
        struct Op
        {
            OpType type;
            std::string from;
            size_t outPort;
            std::string to;
            size_t inPort;
            BusNodePtr node;
            LinkOptions options;
        };

        Bus& m_bus;
        std::vector<Op> m_ops;
        std::string m_error;
        bool m_committed = false;
    };

    [[nodiscard]] Transaction Begin() { return Transaction(*this); }

    // ---- 单步拓扑操作（各自是一个事务）----
    // 注册节点并把事件通道 / 上下文接给它。同名重复注册会替换（旧节点的连线一并拆除）
    void Register(const std::string& name, BusNodePtr node)
    {
        if (!node) {
            return;
        }
        Begin().Register(name, std::move(node)).Commit();
    }

    bool Unregister(const std::string& name) { return Begin().Unregister(name).Commit(); }

    // 把 from 的输出端口 0 直连到 to 的输入端口 0
    bool Link(const std::string& from, const std::string& to, LinkOptions options = {})
    {
        return Begin().Link(from, to, options).Commit();
    }

    bool Link(const std::string& from, size_t outPort, const std::string& to, size_t inPort,
              LinkOptions options = {})
    {
        return Begin().Link(from, outPort, to, inPort, options).Commit();
    }

    bool Unlink(const std::string& from, const std::string& to)
    {
        return Begin().Unlink(from, to).Commit();
    }

    // ---- 分组生命周期 ----
    // 顺序由当前拓扑决定（拓扑排序，同层按名字），不需要调用方手写先后
    struct GroupResult
    {
        bool ok = true;
        std::string failedNode;   // 失败的节点名
        int code = 0;             // Configure 的返回值（<0）
    };

    // 上游先于下游。遇到第一个失败即停止（已配置的不回滚，由调用方 CloseGroup）
    GroupResult ConfigureGroup(NodeGroup group)
    {
        GroupResult result;
        for (const auto& node : GroupNodes(group, /*upstreamFirst=*/true)) {
            const int ret = node->Configure();
            if (ret < 0) {
                result = {false, node->Name(), ret};
                break;
            }
        }
        return result;
    }

    // 下游先于上游，跳过未激活节点与已在运行的节点（预览期间已起的节点在开播时不重复启动）。
    // 任一节点启动失败：把本次已启动的按停止顺序停掉，返回失败节点
    GroupResult StartGroup(NodeGroup group)
    {
        std::vector<BusNodePtr> started;
        for (const auto& node : GroupNodes(group, /*upstreamFirst=*/false)) {
            if (!node->IsActive() || node->Lifecycle() == NodeLifecycle::Running) {
                continue;
            }
            if (!node->StartNode()) {
                TG_LOGE("Bus: %s 组启动失败于节点 %s", NodeGroupName(group), node->Name().c_str());
                for (auto it = started.rbegin(); it != started.rend(); ++it) {
                    (*it)->StopNode();
                }
                return {false, node->Name(), 0};
            }
            started.push_back(node);
        }
        return {};
    }

    // 上游先于下游。对未启动的节点也调用（NodeStop 必须幂等）
    void StopGroup(NodeGroup group)
    {
        for (const auto& node : GroupNodes(group, /*upstreamFirst=*/true)) {
            node->StopNode();
        }
    }

    // 下游先于上游（封装器先写尾，再关编码器）
    void CloseGroup(NodeGroup group)
    {
        for (const auto& node : GroupNodes(group, /*upstreamFirst=*/false)) {
            node->CloseNode();
        }
    }

    // 组内节点按拓扑顺序排列的名字（调试 / 测试用）
    [[nodiscard]] std::vector<std::string> GroupOrder(NodeGroup group, bool upstreamFirst = true) const
    {
        std::vector<std::string> names;
        for (const auto& node : GroupNodes(group, upstreamFirst)) {
            names.push_back(node->Name());
        }
        return names;
    }

    BusNodePtr Get(const std::string& name) const
    {
        auto topo = Snapshot();
        auto it = topo->nodes.find(name);
        return it == topo->nodes.end() ? nullptr : it->second;
    }

    [[nodiscard]] std::vector<std::string> NodeNames() const
    {
        auto topo = Snapshot();
        std::vector<std::string> names;
        names.reserve(topo->nodes.size());
        for (const auto& [name, _] : topo->nodes) {
            names.push_back(name);
        }
        return names;
    }

    // 某节点某输入端口当前的上游（"from:port"），没有返回空
    [[nodiscard]] std::vector<std::string> InputsOf(const std::string& name) const
    {
        auto topo = Snapshot();
        std::vector<std::string> out;
        for (const auto& l : topo->links) {
            if (l.to == name) {
                out.push_back(l.from + ":" + std::to_string(l.outPort) + "->" + std::to_string(l.inPort));
            }
        }
        return out;
    }

    // ---- 数据面 ----
    // 按名字投递到节点的输入端口 0。热路径请缓存 Get() 的结果直接 Submit，避免每帧一次查找
    bool Dispatch(const std::string& entry, FramePtr frame) const
    {
        auto node = Get(entry);
        return node && node->Submit(std::move(frame));
    }

    [[nodiscard]] size_t TotalPending() const
    {
        auto topo = Snapshot();
        size_t total = 0;
        for (const auto& [_, node] : topo->nodes) {
            total += node->Pending();
        }
        for (const auto& l : topo->links) {
            if (l.channel) {
                total += l.channel->Size();
            }
        }
        return total;
    }

    [[nodiscard]] size_t PendingOf(const std::string& name) const
    {
        auto node = Get(name);
        return node ? node->Pending() : 0;
    }

    // 节点图：每个节点一行（"node ..."），每条连线一行（"link ..."），按名字排序，便于 diff
    [[nodiscard]] std::vector<std::string> Dump() const
    {
        auto topo = Snapshot();
        std::vector<std::string> lines;
        std::map<std::string, BusNodePtr> sorted(topo->nodes.begin(), topo->nodes.end());
        for (const auto& [name, node] : sorted) {
            const size_t in = node->InputPorts();
            std::string line = "node " + name + " in=" + (in == kDynamicPorts ? "*" : std::to_string(in)) +
                               " out=" + std::to_string(node->OutputPorts()) +
                               " pending=" + std::to_string(node->Pending());
            if (node->Group() != NodeGroup::None) {
                line += std::string(" group=") + NodeGroupName(node->Group()) +
                        " state=" + NodeLifecycleName(node->Lifecycle());
            }
            const std::string state = node->DumpState();
            if (!state.empty()) {
                line += " " + state;
            }
            lines.push_back(std::move(line));
        }
        std::vector<std::string> links;
        for (const auto& l : topo->links) {
            std::string line = "link " + l.from + ":" + std::to_string(l.outPort) + " -> " + l.to + ":" +
                               std::to_string(l.inPort) + " " + LinkModeName(l.options.mode);
            if (l.options.mode == LinkMode::Queued) {
                line += "[" + std::to_string(l.options.capacity) + "]";
            }
            line += " delivered=" + std::to_string(l.stats->delivered.load()) +
                    " dropped=" + std::to_string(l.stats->dropped.load()) +
                    " rejected=" + std::to_string(l.stats->rejected.load());
            if (l.channel) {
                line += " queued=" + std::to_string(l.channel->Size());
            }
            links.push_back(std::move(line));
        }
        std::sort(links.begin(), links.end());
        lines.insert(lines.end(), links.begin(), links.end());
        return lines;
    }

    [[nodiscard]] std::string DumpGraph() const
    {
        std::string out;
        for (const auto& line : Dump()) {
            out += line;
            out += '\n';
        }
        return out;
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
        auto topo = Snapshot();
        size_t handled = 0;
        for (const auto& [_, node] : topo->nodes) {
            if (node->OnCommand(command)) {
                ++handled;
            }
        }
        return handled;
    }

    // ---- 上下文 ----
    [[nodiscard]] BusContext& Context() noexcept { return *m_context; }
    [[nodiscard]] const std::shared_ptr<BusContext>& ContextPtr() const noexcept { return m_context; }

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

    // 宽限期上限：删除连线时最多等上游这么久把正在进行的投递做完
    static constexpr std::chrono::milliseconds kGracePeriod{2000};

private:
    struct LinkRec
    {
        uint64_t id = 0;
        std::string from;
        size_t outPort = 0;
        std::string to;
        size_t inPort = 0;
        LinkOptions options;
        std::shared_ptr<LinkStats> stats;
        std::shared_ptr<LinkChannel> channel;
    };

    struct Topology
    {
        std::unordered_map<std::string, BusNodePtr> nodes;
        std::vector<LinkRec> links;
    };

    [[nodiscard]] std::shared_ptr<const Topology> Snapshot() const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_topo;
    }

    // 全图拓扑排序（Kahn，同层按名字），再筛出指定组。注意节点在拓扑里的名字可能与 Name() 不同，
    // 排序按注册名
    [[nodiscard]] std::vector<BusNodePtr> GroupNodes(NodeGroup group, bool upstreamFirst) const
    {
        auto topo = Snapshot();
        std::map<std::string, int> indegree;
        std::map<std::string, std::vector<std::string>> adj;
        for (const auto& [name, _] : topo->nodes) {
            indegree[name] = 0;
        }
        for (const auto& l : topo->links) {
            adj[l.from].push_back(l.to);
            indegree[l.to]++;
        }
        std::set<std::string> ready;
        for (const auto& [name, deg] : indegree) {
            if (deg == 0) {
                ready.insert(name);
            }
        }
        std::vector<BusNodePtr> order;
        while (!ready.empty()) {
            const std::string name = *ready.begin();
            ready.erase(ready.begin());
            const auto& node = topo->nodes.at(name);
            if (node->Group() == group) {
                order.push_back(node);
            }
            for (const auto& next : adj[name]) {
                if (--indegree[next] == 0) {
                    ready.insert(next);
                }
            }
        }
        if (!upstreamFirst) {
            std::reverse(order.begin(), order.end());
        }
        return order;
    }

    static bool HasCycle(const Topology& topo)
    {
        std::unordered_map<std::string, std::vector<std::string>> adj;
        for (const auto& l : topo.links) {
            adj[l.from].push_back(l.to);
        }
        std::unordered_map<std::string, int> color;   // 0 未访问 1 栈中 2 完成
        std::function<bool(const std::string&)> dfs = [&](const std::string& n) {
            color[n] = 1;
            for (const auto& m : adj[n]) {
                const int c = color[m];
                if (c == 1 || (c == 0 && dfs(m))) {
                    return true;
                }
            }
            color[n] = 2;
            return false;
        };
        for (const auto& [name, _] : topo.nodes) {
            if (color[name] == 0 && dfs(name)) {
                return true;
            }
        }
        return false;
    }

    static std::shared_ptr<const BusRouteTable> BuildRoutes(const Topology& topo, const std::string& name,
                                                            const BusNodePtr& node)
    {
        auto table = std::make_shared<BusRouteTable>(std::max<size_t>(node->OutputPorts(), 1));
        for (const auto& l : topo.links) {
            if (l.from != name) {
                continue;
            }
            (*table)[l.outPort].push_back(BusRoute{topo.nodes.at(l.to), l.inPort, l.channel, l.stats});
        }
        return table;
    }

    bool CommitOps(const std::vector<Transaction::Op>& ops, std::string& error)
    {
        if (t_inTopologyCallback) {
            error = "不能在 OnInputAttached / OnInputDetached 回调里提交拓扑事务";
            TG_LOGE("Bus: %s", error.c_str());
            return false;
        }
        std::lock_guard<std::mutex> commitLock(m_commitMutex);
        auto old = Snapshot();
        Topology next = *old;

        auto fail = [&](std::string why) {
            error = std::move(why);
            TG_LOGE("Bus: 拓扑事务被拒绝：%s", error.c_str());
            return false;
        };
        auto removeLinksOf = [&](const std::string& name) {
            next.links.erase(std::remove_if(next.links.begin(), next.links.end(),
                                            [&](const LinkRec& l) { return l.from == name || l.to == name; }),
                             next.links.end());
        };

        for (const auto& op : ops) {
            switch (op.type) {
                case Transaction::OpType::Register: {
                    if (!op.node || op.from.empty()) {
                        return fail("Register: 节点为空或名字为空");
                    }
                    if (next.nodes.count(op.from) != 0) {
                        removeLinksOf(op.from);
                    }
                    next.nodes[op.from] = op.node;
                    break;
                }
                case Transaction::OpType::Unregister: {
                    if (next.nodes.erase(op.from) == 0) {
                        return fail("Unregister: 节点不存在 " + op.from);
                    }
                    removeLinksOf(op.from);
                    break;
                }
                case Transaction::OpType::Link: {
                    auto src = next.nodes.find(op.from);
                    auto dst = next.nodes.find(op.to);
                    if (src == next.nodes.end() || dst == next.nodes.end()) {
                        return fail("Link: 节点不存在 " + op.from + " -> " + op.to);
                    }
                    if (src->second == dst->second) {
                        return fail("Link: 不能自环 " + op.from);
                    }
                    if (op.outPort >= src->second->OutputPorts()) {
                        return fail("Link: 输出端口越界 " + op.from + ":" + std::to_string(op.outPort));
                    }
                    const size_t inPorts = dst->second->InputPorts();
                    size_t inPort = op.inPort;
                    std::set<size_t> used;
                    for (const auto& l : next.links) {
                        if (l.to == op.to) {
                            used.insert(l.inPort);
                        }
                    }
                    if (inPort == kAnyPort) {
                        inPort = 0;
                        while (used.count(inPort) != 0) {
                            ++inPort;
                        }
                    }
                    if (inPorts != kDynamicPorts && inPort >= inPorts) {
                        return fail("Link: 输入端口越界 " + op.to + ":" + std::to_string(inPort));
                    }
                    for (const auto& l : next.links) {
                        if (l.from == op.from && l.outPort == op.outPort && l.to == op.to && l.inPort == inPort) {
                            return fail("Link: 重复连线 " + op.from + " -> " + op.to);
                        }
                    }
                    LinkRec rec;
                    rec.id = ++m_nextLinkId;
                    rec.from = op.from;
                    rec.outPort = op.outPort;
                    rec.to = op.to;
                    rec.inPort = inPort;
                    rec.options = op.options;
                    rec.stats = std::make_shared<LinkStats>();
                    next.links.push_back(std::move(rec));
                    break;
                }
                case Transaction::OpType::Unlink: {
                    const size_t before = next.links.size();
                    next.links.erase(
                        std::remove_if(next.links.begin(), next.links.end(),
                                       [&](const LinkRec& l) {
                                           return l.from == op.from && l.to == op.to &&
                                                  (op.outPort == kAnyPort || l.outPort == op.outPort) &&
                                                  (op.inPort == kAnyPort || l.inPort == op.inPort);
                                       }),
                        next.links.end());
                    if (next.links.size() == before) {
                        return fail("Unlink: 连线不存在 " + op.from + " -> " + op.to);
                    }
                    break;
                }
            }
        }
        if (HasCycle(next)) {
            return fail("拓扑成环");
        }

        // ---- 校验通过，开始生效 ----
        for (const auto& op : ops) {
            if (op.type == Transaction::OpType::Register) {
                op.node->AttachEvents(m_events);
                op.node->AttachContext(m_context);
            }
        }
        auto idsOf = [](const std::vector<LinkRec>& v) {
            std::set<uint64_t> s;
            for (const auto& l : v) {
                s.insert(l.id);
            }
            return s;
        };
        const auto oldIds = idsOf(old->links);
        const auto newIds = idsOf(next.links);
        std::vector<LinkRec> added;
        std::vector<LinkRec> removed;
        for (auto& l : next.links) {
            if (oldIds.count(l.id) == 0) {
                if (l.options.mode != LinkMode::Direct) {
                    l.channel = std::make_shared<LinkChannel>(next.nodes.at(l.to), l.inPort, l.options, l.stats);
                    l.channel->Start();
                }
                added.push_back(l);
            }
        }
        for (const auto& l : old->links) {
            if (newIds.count(l.id) == 0) {
                removed.push_back(l);
            }
        }

        // 新增连线：先通知下游准备好输入，再让上游开始投递
        {
            CallbackScope scope;
            for (const auto& l : added) {
                next.nodes.at(l.to)->OnInputAttached(l.inPort, l.from);
            }
        }

        auto published = std::make_shared<const Topology>(std::move(next));
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_topo = published;
        }

        // 重建受影响节点的路由：新注册的、出边有增删的；被注销 / 被替换的节点清空路由
        std::set<std::string> touched;
        for (const auto& l : added) {
            touched.insert(l.from);
        }
        for (const auto& l : removed) {
            touched.insert(l.from);
        }
        std::vector<BusNodePtr> retired;
        for (const auto& [name, node] : old->nodes) {
            auto it = published->nodes.find(name);
            if (it == published->nodes.end() || it->second != node) {
                retired.push_back(node);
            }
        }
        for (const auto& op : ops) {
            if (op.type == Transaction::OpType::Register) {
                touched.insert(op.from);
            }
        }
        for (const auto& name : touched) {
            auto it = published->nodes.find(name);
            if (it != published->nodes.end()) {
                it->second->SetRoutes(BuildRoutes(*published, name, it->second));
            }
        }
        for (const auto& node : retired) {
            node->SetRoutes(nullptr);
        }

        // 删除连线：宽限期（上游可能还拿着旧路由表在投递）→ 停连线线程 → 通知下游
        if (!removed.empty()) {
            std::set<BusNode*> waited;
            for (const auto& l : removed) {
                auto it = old->nodes.find(l.from);
                if (it != old->nodes.end() && waited.insert(it->second.get()).second) {
                    it->second->WaitQuiescent(kGracePeriod);
                }
            }
            for (const auto& l : removed) {
                if (l.channel) {
                    l.channel->Stop();
                }
            }
            CallbackScope scope;
            for (const auto& l : removed) {
                auto it = old->nodes.find(l.to);
                if (it != old->nodes.end()) {
                    it->second->OnInputDetached(l.inPort, l.from);
                }
            }
        }
        return true;
    }

    struct CallbackScope
    {
        CallbackScope() { t_inTopologyCallback = true; }
        ~CallbackScope() { t_inTopologyCallback = false; }
    };

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

    inline static thread_local bool t_inTopologyCallback = false;

    mutable std::shared_mutex m_mutex;    // 只保护 m_topo 指针本身
    std::mutex m_commitMutex;             // 事务之间互斥
    std::shared_ptr<const Topology> m_topo = std::make_shared<const Topology>();
    uint64_t m_nextLinkId = 0;
    std::shared_ptr<EventHub> m_events;
    std::shared_ptr<BusContext> m_context;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_BUS_H
