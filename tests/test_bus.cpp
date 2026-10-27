// 总线三个平面的行为：数据（节点链）/ 命令 / 事件（含线程与死锁相关的边界）
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "core/Bus.h"
#include "tg_test.h"

using namespace TGCapture::core;
using namespace std::chrono_literals;

namespace
{

class TestNode : public BusNode
{
public:
    explicit TestNode(std::string name) : BusNode(std::move(name)) {}

    bool Submit(FramePtr frame) override
    {
        ++submitted;
        return Forward(std::move(frame));
    }
    size_t Pending() const override { return pending; }
    bool OnCommand(const BusCommand& command) override
    {
        ++commands;
        lastCommand = command.type;
        return command.type != BusCommandType::Abort;   // Abort 当作"不认识"
    }

    void Fail(int code) { ReportError(code, "boom", 7); }
    void Note(int type, int64_t data = 0) { ReportNotify(type, "note", data); }

    std::atomic<int> submitted{0};
    std::atomic<int> commands{0};
    std::atomic<size_t> pending{0};
    BusCommandType lastCommand = BusCommandType::Flush;
};

// 线程安全的事件收集器
struct Collector
{
    std::mutex mutex;
    std::vector<BusEvent> events;
    std::vector<std::thread::id> threads;

    EventHub::Handler Handler()
    {
        return [this](const BusEvent& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
            threads.push_back(std::this_thread::get_id());
        };
    }
    size_t Count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return events.size();
    }
};

}  // namespace

// ------------------------------------------------------------------ 数据面
TG_TEST(Bus, DataFlowsThroughLinkedChain)
{
    Bus bus;
    auto a = std::make_shared<TestNode>("a");
    auto b = std::make_shared<TestNode>("b");
    auto c = std::make_shared<TestNode>("c");
    bus.Register("a", a);
    bus.Register("b", b);
    bus.Register("c", c);
    TG_ASSERT(bus.Link("a", "b"));
    TG_ASSERT(bus.Link("b", "c"));

    for (int i = 0; i < 3; ++i) {
        TG_EXPECT(a->Submit(std::make_shared<Frame>()));
    }
    TG_EXPECT_EQ(a->submitted.load(), 3);
    TG_EXPECT_EQ(b->submitted.load(), 3);
    TG_EXPECT_EQ(c->submitted.load(), 3);
}

TG_TEST(Bus, TwoUpstreamsCanShareOneDownstream)
{
    Bus bus;
    auto v = std::make_shared<TestNode>("v");
    auto au = std::make_shared<TestNode>("au");
    auto mux = std::make_shared<TestNode>("mux");
    bus.Register("v", v);
    bus.Register("au", au);
    bus.Register("mux", mux);
    TG_ASSERT(bus.Link("v", "mux") && bus.Link("au", "mux"));
    v->Submit(std::make_shared<Frame>());
    au->Submit(std::make_shared<Frame>());
    au->Submit(std::make_shared<Frame>());
    TG_EXPECT_EQ(mux->submitted.load(), 3);
}

TG_TEST(Bus, LinkRejectsUnknownAndSelf)
{
    Bus bus;
    bus.Register("a", std::make_shared<TestNode>("a"));
    TG_EXPECT(!bus.Link("a", "nope"));
    TG_EXPECT(!bus.Link("nope", "a"));
    TG_EXPECT(!bus.Link("a", "a"));
}

TG_TEST(Bus, DispatchByNameAndMissingEntry)
{
    Bus bus;
    auto a = std::make_shared<TestNode>("a");
    bus.Register("a", a);
    TG_EXPECT(bus.Dispatch("a", std::make_shared<Frame>()));
    TG_EXPECT(!bus.Dispatch("missing", std::make_shared<Frame>()));
    TG_EXPECT_EQ(a->submitted.load(), 1);
}

TG_TEST(Bus, PendingAggregation)
{
    Bus bus;
    auto a = std::make_shared<TestNode>("a");
    auto b = std::make_shared<TestNode>("b");
    a->pending = 3;
    b->pending = 4;
    bus.Register("a", a);
    bus.Register("b", b);
    TG_EXPECT_EQ(bus.TotalPending(), size_t{7});
    TG_EXPECT_EQ(bus.PendingOf("a"), size_t{3});
    TG_EXPECT_EQ(bus.PendingOf("missing"), size_t{0});
    TG_EXPECT_EQ(bus.Dump().size(), size_t{2});
}

// ------------------------------------------------------------------ 命令面
TG_TEST(Bus, SendReportsHandledUnhandledAndMissing)
{
    Bus bus;
    auto a = std::make_shared<TestNode>("a");
    bus.Register("a", a);
    TG_EXPECT(bus.Send("a", BusCommand{BusCommandType::Flush, 0}));
    TG_EXPECT(!bus.Send("a", BusCommand{BusCommandType::Abort, 0}));   // 节点不处理
    TG_EXPECT(!bus.Send("missing", BusCommand{BusCommandType::Flush, 0}));
    TG_EXPECT_EQ(a->commands.load(), 2);
}

TG_TEST(Bus, BroadcastCountsHandlers)
{
    Bus bus;
    for (const char* n : {"a", "b", "c"}) {
        bus.Register(n, std::make_shared<TestNode>(n));
    }
    TG_EXPECT_EQ(bus.Broadcast(BusCommand{BusCommandType::Flush, 0}), size_t{3});
    TG_EXPECT_EQ(bus.Broadcast(BusCommand{BusCommandType::Abort, 0}), size_t{0});
}

TG_TEST(Bus, DefaultNodeIgnoresCommands)
{
    // 没覆写 OnCommand 的节点必须返回 false，而不是默默吞掉
    struct Plain : BusNode
    {
        Plain() : BusNode("plain") {}
        bool Submit(FramePtr) override { return true; }
        size_t Pending() const override { return 0; }
    };
    Plain p;
    TG_EXPECT(!p.OnCommand(BusCommand{BusCommandType::Flush, 0}));
}

// ------------------------------------------------------------------ 事件面
TG_TEST(BusEvents, DeliveredInOrderOnDispatchThread)
{
    Bus bus;
    auto node = std::make_shared<TestNode>("n");
    bus.Register("n", node);
    Collector c;
    bus.Subscribe(c.Handler());

    for (int i = 0; i < 100; ++i) {
        node->Fail(i);
    }
    TG_ASSERT(bus.Events().Drain(2000ms));
    TG_ASSERT(c.Count() == 100);
    for (int i = 0; i < 100; ++i) {
        TG_EXPECT_EQ(c.events[i].code, i);
    }
    TG_EXPECT_EQ(c.events[0].source, std::string("n"));
    TG_EXPECT_EQ(c.events[0].extra, 7);
    TG_EXPECT(c.events[0].type == BusEventType::Error);
    TG_EXPECT(c.threads[0] != std::this_thread::get_id());   // 不在发布者线程
}

TG_TEST(BusEvents, PublishNeverBlocksOnSlowSubscriber)
{
    Bus bus;
    auto node = std::make_shared<TestNode>("n");
    bus.Register("n", node);
    bus.Subscribe([](const BusEvent&) { std::this_thread::sleep_for(40ms); });

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        node->Note(i);
    }
    const auto spent = std::chrono::steady_clock::now() - t0;
    // 订阅者总共要 800ms；发布者必须几乎立即返回
    TG_EXPECT(spent < 100ms);
    bus.Events().Shutdown();
}

TG_TEST(BusEvents, SyncPublishRunsOnCallerThread)
{
    Bus bus;
    Collector c;
    bus.Subscribe(c.Handler());
    bus.PublishSync(BusEventType::Prepared, "capturer", 0, {});
    TG_ASSERT(c.Count() == 1);   // 返回时已经投递完
    TG_EXPECT(c.threads[0] == std::this_thread::get_id());
}

TG_TEST(BusEvents, ErrorsSurviveQueueOverflow)
{
    auto hub = EventHub::Create(4);
    Collector c;
    std::promise<void> gate;
    auto gateFuture = gate.get_future().share();
    std::atomic<bool> first{true};
    hub->Subscribe([&](const BusEvent& e) {
        if (first.exchange(false)) {
            gateFuture.wait();   // 卡住分发线程，让后面的事件在队列里堆积
        }
        std::lock_guard<std::mutex> lock(c.mutex);
        c.events.push_back(e);
    });

    BusEvent blocker;
    blocker.type = BusEventType::Notify;
    hub->Publish(blocker);
    std::this_thread::sleep_for(50ms);   // 确保分发线程已经取走它并卡在 gate 上

    BusEvent error;
    error.type = BusEventType::Error;
    error.code = -101;
    hub->Publish(error);
    for (int i = 0; i < 30; ++i) {
        BusEvent n;
        n.type = BusEventType::Notify;
        n.code = i;
        hub->Publish(n);
    }
    TG_EXPECT(hub->Dropped() > 0);

    gate.set_value();
    TG_ASSERT(hub->Drain(2000ms));
    bool gotError = false;
    for (const auto& e : c.events) {
        gotError |= (e.type == BusEventType::Error && e.code == -101);
    }
    TG_EXPECT(gotError);   // 队列溢出丢的是通知，不是错误
    hub->Shutdown();
}

TG_TEST(BusEvents, UnsubscribeStopsDelivery)
{
    auto hub = EventHub::Create();
    Collector c;
    const auto id = hub->Subscribe(c.Handler());
    BusEvent e;
    hub->PublishSync(e);
    hub->Unsubscribe(id);
    hub->PublishSync(e);
    TG_EXPECT_EQ(c.Count(), size_t{1});
    hub->Shutdown();
}

TG_TEST(BusEvents, SubscriberExceptionDoesNotKillDispatcher)
{
    auto hub = EventHub::Create();
    hub->Subscribe([](const BusEvent&) { throw std::runtime_error("host bug"); });
    Collector c;
    hub->Subscribe(c.Handler());   // 排在抛异常的订阅者之后，仍必须被调用
    BusEvent e;
    hub->Publish(e);
    hub->Publish(e);
    TG_ASSERT(hub->Drain(2000ms));
    TG_EXPECT_EQ(c.Count(), size_t{2});
    hub->Shutdown();
}

TG_TEST(BusEvents, DrainFromDispatchThreadReturnsInsteadOfDeadlocking)
{
    auto hub = EventHub::Create();
    std::promise<bool> result;
    hub->Subscribe([&](const BusEvent&) { result.set_value(hub->Drain(5000ms)); });
    hub->Publish(BusEvent{});
    auto fut = result.get_future();
    TG_ASSERT(fut.wait_for(2000ms) == std::future_status::ready);
    TG_EXPECT(fut.get());
    hub->Shutdown();
}

TG_TEST(BusEvents, ShutdownFromInsideSubscriberDoesNotDeadlock)
{
    // 宿主在回调里销毁整个对象 → 分发线程要关闭"自己所在的"事件中心
    auto hub = EventHub::Create();
    std::promise<void> done;
    hub->Subscribe([&](const BusEvent&) {
        hub->Shutdown();   // 在分发线程内：必须分离而不是 join 自己
        done.set_value();
    });
    hub->Publish(BusEvent{});
    auto fut = done.get_future();
    TG_ASSERT(fut.wait_for(2000ms) == std::future_status::ready);
    hub->Shutdown();   // 幂等
    hub.reset();
    std::this_thread::sleep_for(50ms);   // 给被分离的线程收尾的时间（ASAN/TSAN 下才能看出问题）
}

TG_TEST(BusEvents, PublishAfterShutdownIsNoop)
{
    auto hub = EventHub::Create();
    Collector c;
    hub->Subscribe(c.Handler());
    hub->Shutdown();
    hub->Publish(BusEvent{});
    hub->PublishSync(BusEvent{});
    std::this_thread::sleep_for(30ms);
    TG_EXPECT_EQ(c.Count(), size_t{0});
}

TG_TEST(BusEvents, NodeWithoutBusReportsSilently)
{
    TestNode lonely("lonely");
    lonely.Fail(1);          // 没挂总线：不崩、不阻塞
    lonely.Note(2);
    TG_EXPECT(true);
}

TG_TEST(BusEvents, StopThenStoppedOrderingViaDrain)
{
    // 模拟 TGCapturer::Stop：运行期错误先被投递，Drain 之后再同步发 Stopped
    Bus bus;
    auto node = std::make_shared<TestNode>("n");
    bus.Register("n", node);
    Collector c;
    bus.Subscribe(c.Handler());
    node->Fail(-101);
    bus.Events().Drain(2000ms);
    bus.PublishSync(BusEventType::Stopped, "capturer", 0, {});
    TG_ASSERT(c.Count() == 2);
    TG_EXPECT(c.events[0].type == BusEventType::Error);
    TG_EXPECT(c.events[1].type == BusEventType::Stopped);
}
