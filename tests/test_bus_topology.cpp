// Bus 2.0：端口 / 运行期拓扑事务 / 三种连线 / 带内控制帧 / Dump / 上下文
// 并发用例（*Stress*）用于 TSan：运行期增删连线时生产者持续投递，删除后下游不得再收到帧
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/Bus.h"
#include "tg_test.h"

using namespace TGCapture::core;
using namespace std::chrono_literals;

namespace
{

// 多端口节点：输入按端口计数，Submit 原样转发到输出端口 outPort
class PortNode : public BusNode
{
public:
    PortNode(std::string name, size_t inPorts, size_t outPorts)
        : BusNode(std::move(name)), m_in(inPorts), m_out(outPorts)
    {
    }

    size_t InputPorts() const override { return m_in; }
    size_t OutputPorts() const override { return m_out; }

    bool Submit(FramePtr frame) override { return SubmitInput(0, std::move(frame)); }

    bool SubmitInput(size_t port, FramePtr frame) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (detachedPorts.count(port) != 0) {
                ++afterDetach;   // 断开之后还收到帧 = 违反保证
            }
            perPort[port]++;
            order.push_back(frame->control);
            threads.insert(std::this_thread::get_id());
        }
        ++received;
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        return accept;
    }

    size_t Pending() const override { return 0; }

    void OnInputAttached(size_t port, const std::string& from) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        detachedPorts.erase(port);
        attached.push_back(from + "@" + std::to_string(port));
        if (tryNestedCommit && bus) {
            nestedResult = bus->Link("x", "y");
        }
    }

    void OnInputDetached(size_t port, const std::string& from) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        detachedPorts.insert(port);
        detached.push_back(from + "@" + std::to_string(port));
    }

    bool Out(size_t port, FramePtr f) { return Emit(port, std::move(f)); }
    size_t Downstream(size_t port) const { return DownstreamPending(port); }
    bool Has(size_t port) const { return HasDownstream(port); }
    std::shared_ptr<BusContext> Ctx() const { return Context(); }

    int Count(size_t port)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return perPort[port];
    }

    std::mutex mutex;
    std::map<size_t, int> perPort;
    std::set<size_t> detachedPorts;
    std::vector<ControlKind> order;
    std::set<std::thread::id> threads;
    std::vector<std::string> attached;
    std::vector<std::string> detached;
    std::atomic<int> received{0};
    std::atomic<int> afterDetach{0};
    std::chrono::milliseconds delay{0};
    bool accept = true;

    bool tryNestedCommit = false;
    Bus* bus = nullptr;
    bool nestedResult = true;

private:
    size_t m_in;
    size_t m_out;
};

FramePtr Data() { return std::make_shared<Frame>(); }

template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------ 端口与扇出
TG_TEST(BusTopology, OutputPortsRouteIndependently)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 2);
    auto a = std::make_shared<PortNode>("a", 1, 1);
    auto b = std::make_shared<PortNode>("b", 1, 1);
    bus.Register("src", src);
    bus.Register("a", a);
    bus.Register("b", b);
    TG_ASSERT(bus.Link("src", 0, "a", 0));
    TG_ASSERT(bus.Link("src", 1, "b", 0));
    TG_EXPECT(!bus.Link("src", 2, "b", 0));   // 输出端口越界
    TG_EXPECT(!bus.Link("src", 0, "a", 1));   // 输入端口越界

    src->Out(0, Data());
    src->Out(1, Data());
    src->Out(1, Data());
    TG_EXPECT_EQ(a->received.load(), 1);
    TG_EXPECT_EQ(b->received.load(), 2);
    TG_EXPECT(src->Has(0) && src->Has(1));
}

TG_TEST(BusTopology, FanOutDeliversToAllAndReportsRejection)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto a = std::make_shared<PortNode>("a", 1, 1);
    auto b = std::make_shared<PortNode>("b", 1, 1);
    bus.Register("src", src);
    bus.Register("a", a);
    bus.Register("b", b);
    TG_ASSERT(bus.Link("src", "a") && bus.Link("src", "b"));
    TG_EXPECT(src->Out(0, Data()));
    TG_EXPECT_EQ(a->received.load(), 1);
    TG_EXPECT_EQ(b->received.load(), 1);

    b->accept = false;
    TG_EXPECT(!src->Out(0, Data()));   // 任一下游拒绝 → false，但其余下游照常收到
    TG_EXPECT_EQ(a->received.load(), 2);
}

TG_TEST(BusTopology, DynamicInputsGetFreePortsAndCallbacks)
{
    Bus bus;
    auto mix = std::make_shared<PortNode>("mix", kDynamicPorts, 1);
    bus.Register("mix", mix);
    for (const char* n : {"s0", "s1", "s2"}) {
        bus.Register(n, std::make_shared<PortNode>(n, 1, 1));
        TG_ASSERT(bus.Link(n, 0, "mix", kAnyPort));
    }
    TG_EXPECT_EQ(mix->attached.size(), size_t{3});
    TG_EXPECT(mix->attached[2] == "s2@2");

    TG_ASSERT(bus.Unlink("s1", "mix"));
    TG_EXPECT_EQ(mix->detached.size(), size_t{1});
    TG_EXPECT(mix->detached[0] == "s1@1");

    // 空出来的端口被复用
    bus.Register("s3", std::make_shared<PortNode>("s3", 1, 1));
    TG_ASSERT(bus.Link("s3", 0, "mix", kAnyPort));
    TG_EXPECT(mix->attached.back() == "s3@1");
    TG_EXPECT_EQ(bus.InputsOf("mix").size(), size_t{3});
}

// ------------------------------------------------------------------ 事务
TG_TEST(BusTopology, FailedTransactionChangesNothing)
{
    Bus bus;
    auto a = std::make_shared<PortNode>("a", 1, 1);
    auto b = std::make_shared<PortNode>("b", 1, 1);
    bus.Register("a", a);
    bus.Register("b", b);
    auto c = std::make_shared<PortNode>("c", 1, 1);

    auto tx = bus.Begin();
    tx.Register("c", c).Link("a", "b").Link("b", "missing");
    TG_EXPECT(!tx.Commit());
    TG_EXPECT(!tx.Error().empty());
    TG_EXPECT(bus.Get("c") == nullptr);
    TG_EXPECT(!a->Has(0));
    TG_EXPECT(b->attached.empty());
    TG_EXPECT(!tx.Commit());   // 不能重复提交
}

TG_TEST(BusTopology, RejectsCyclesDuplicatesAndMissingUnlink)
{
    Bus bus;
    for (const char* n : {"a", "b", "c"}) {
        bus.Register(n, std::make_shared<PortNode>(n, 1, 1));
    }
    TG_ASSERT(bus.Link("a", "b") && bus.Link("b", "c"));
    TG_EXPECT(!bus.Link("c", "a"));   // 成环
    TG_EXPECT(!bus.Link("a", "b"));   // 重复
    TG_EXPECT(!bus.Unlink("a", "c"));
    TG_EXPECT(!bus.Unregister("zzz"));
}

TG_TEST(BusTopology, UnregisterDropsLinksAndNotifiesDownstream)
{
    Bus bus;
    auto a = std::make_shared<PortNode>("a", 1, 1);
    auto b = std::make_shared<PortNode>("b", 1, 1);
    bus.Register("a", a);
    bus.Register("b", b);
    TG_ASSERT(bus.Link("a", "b"));
    TG_ASSERT(bus.Unregister("a"));
    TG_EXPECT_EQ(b->detached.size(), size_t{1});
    TG_EXPECT(!a->Has(0));            // 被注销节点的路由被清空
    TG_EXPECT(a->Out(0, Data()));     // 链尾语义：没下游返回 true
    TG_EXPECT_EQ(b->received.load(), 0);
}

TG_TEST(BusTopology, ReplaceOnReRegisterDropsOldLinks)
{
    Bus bus;
    auto a1 = std::make_shared<PortNode>("a", 1, 1);
    auto b = std::make_shared<PortNode>("b", 1, 1);
    bus.Register("a", a1);
    bus.Register("b", b);
    TG_ASSERT(bus.Link("a", "b"));
    auto a2 = std::make_shared<PortNode>("a", 1, 1);
    bus.Register("a", a2);
    TG_EXPECT(bus.Get("a") == a2);
    TG_EXPECT(!a1->Has(0));
    TG_EXPECT(!a2->Has(0));
}

TG_TEST(BusTopology, NestedCommitInCallbackIsRejectedNotDeadlocked)
{
    Bus bus;
    auto x = std::make_shared<PortNode>("x", 1, 1);
    auto y = std::make_shared<PortNode>("y", 1, 1);
    auto m = std::make_shared<PortNode>("m", kDynamicPorts, 1);
    m->tryNestedCommit = true;
    m->bus = &bus;
    bus.Register("x", x);
    bus.Register("y", y);
    bus.Register("m", m);
    TG_ASSERT(bus.Link("x", 0, "m", kAnyPort));
    TG_EXPECT(!m->nestedResult);
    TG_EXPECT(bus.InputsOf("y").empty());
}

// ------------------------------------------------------------------ 连线方式
TG_TEST(BusLink, QueuedDeliversOnItsOwnThread)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto dst = std::make_shared<PortNode>("dst", 1, 1);
    bus.Register("src", src);
    bus.Register("dst", dst);
    TG_ASSERT(bus.Link("src", "dst", LinkOptions::Queued(16)));
    for (int i = 0; i < 10; ++i) {
        TG_EXPECT(src->Out(0, Data()));
    }
    TG_ASSERT(WaitFor([&] { return dst->received.load() == 10; }));
    std::lock_guard<std::mutex> lock(dst->mutex);
    TG_EXPECT(dst->threads.count(std::this_thread::get_id()) == 0);
}

TG_TEST(BusLink, QueuedDropsOldestDataButNeverControl)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto dst = std::make_shared<PortNode>("dst", 1, 1);
    dst->delay = 30ms;   // 慢下游
    bus.Register("src", src);
    bus.Register("dst", dst);
    TG_ASSERT(bus.Link("src", "dst", LinkOptions::Queued(2)));

    src->Out(0, Data());   // 被投递线程取走，正在 sleep
    TG_ASSERT(WaitFor([&] { return dst->received.load() == 1; }));
    src->Out(0, MakeControlFrame(ControlKind::FormatChanged, MediaKind::Video));
    for (int i = 0; i < 6; ++i) {
        src->Out(0, Data());
    }
    src->Out(0, MakeControlFrame(ControlKind::EOS, MediaKind::Video));
    TG_ASSERT(WaitFor([&] {
        std::lock_guard<std::mutex> lock(dst->mutex);
        return !dst->order.empty() && dst->order.back() == ControlKind::EOS;
    }));
    std::lock_guard<std::mutex> lock(dst->mutex);
    int controls = 0;
    int data = 0;
    for (auto k : dst->order) {
        (k == ControlKind::None ? data : controls)++;
    }
    TG_EXPECT_EQ(controls, 2);
    TG_EXPECT_EQ(data, 3);   // 1 个已在投递 + 队列容量 2
    TG_EXPECT(dst->order[1] == ControlKind::FormatChanged);   // 控制帧保序
    const auto dump = bus.DumpGraph();
    TG_EXPECT(dump.find("dropped=4") != std::string::npos);
}

TG_TEST(BusLink, BlockPolicyWaitsForSpace)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto dst = std::make_shared<PortNode>("dst", 1, 1);
    dst->delay = 5ms;
    bus.Register("src", src);
    bus.Register("dst", dst);
    auto opt = LinkOptions::Queued(1, QueueFullPolicy::Block);
    opt.blockTimeout = 500ms;
    TG_ASSERT(bus.Link("src", "dst", opt));
    for (int i = 0; i < 20; ++i) {
        src->Out(0, Data());
    }
    TG_ASSERT(WaitFor([&] { return dst->received.load() == 20; }));   // 一帧不丢
}

TG_TEST(BusLink, LatestKeepsOnlyNewest)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto dst = std::make_shared<PortNode>("dst", 1, 1);
    dst->delay = 20ms;
    bus.Register("src", src);
    bus.Register("dst", dst);
    TG_ASSERT(bus.Link("src", "dst", LinkOptions::Latest()));
    for (int i = 0; i < 50; ++i) {
        auto f = Data();
        f->ptsUs = i;
        src->Out(0, f);
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(60ms);
    TG_EXPECT(dst->received.load() < 20);
    TG_EXPECT(dst->received.load() >= 2);
}

TG_TEST(BusLink, DownstreamPendingIncludesLinkQueue)
{
    Bus bus;
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto dst = std::make_shared<PortNode>("dst", 1, 1);
    dst->delay = 50ms;
    bus.Register("src", src);
    bus.Register("dst", dst);
    TG_ASSERT(bus.Link("src", "dst", LinkOptions::Queued(8)));
    for (int i = 0; i < 4; ++i) {
        src->Out(0, Data());
    }
    TG_EXPECT(src->Downstream(0) >= 3);
    TG_EXPECT(bus.TotalPending() >= 3);
}

// ------------------------------------------------------------------ 运行期增删的保证（TSan）
TG_TEST(BusTopology, StressUnlinkWhileProducingNeverDeliversAfterDetach)
{
    for (LinkOptions opt : {LinkOptions::Direct(), LinkOptions::Queued(4), LinkOptions::Latest()}) {
        Bus bus;
        auto src = std::make_shared<PortNode>("src", 1, 1);
        auto mix = std::make_shared<PortNode>("mix", kDynamicPorts, 1);
        bus.Register("src", src);
        bus.Register("mix", mix);

        std::atomic<bool> stop{false};
        std::vector<std::thread> producers;
        for (int t = 0; t < 3; ++t) {
            producers.emplace_back([&] {
                while (!stop.load()) {
                    src->Out(0, Data());
                }
            });
        }
        for (int round = 0; round < 40; ++round) {
            TG_ASSERT(bus.Begin().Link("src", 0, "mix", 0, opt).Commit());
            std::this_thread::sleep_for(200us);
            TG_ASSERT(bus.Unlink("src", "mix"));
        }
        stop = true;
        for (auto& t : producers) {
            t.join();
        }
        TG_EXPECT_EQ(mix->afterDetach.load(), 0);
        TG_EXPECT_EQ(mix->attached.size(), size_t{40});
        TG_EXPECT_EQ(mix->detached.size(), size_t{40});
    }
}

TG_TEST(BusTopology, StressConcurrentCommitsAndReaders)
{
    Bus bus;
    auto hub = std::make_shared<PortNode>("hub", kDynamicPorts, 1);
    bus.Register("hub", hub);
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load()) {
            (void)bus.Dump();
            (void)bus.TotalPending();
            (void)bus.Get("hub");
        }
    });
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w] {
            for (int i = 0; i < 30; ++i) {
                const std::string name = "n" + std::to_string(w) + "_" + std::to_string(i);
                auto n = std::make_shared<PortNode>(name, 1, 1);
                bus.Begin().Register(name, n).Link(name, 0, "hub", kAnyPort).Commit();
                n->Out(0, Data());
                bus.Unregister(name);
            }
        });
    }
    for (auto& t : writers) {
        t.join();
    }
    stop = true;
    reader.join();
    TG_EXPECT_EQ(hub->received.load(), 120);
    TG_EXPECT_EQ(hub->afterDetach.load(), 0);
    TG_EXPECT_EQ(bus.NodeNames().size(), size_t{1});
}

TG_TEST(BusTopology, NodeOutlivingBusEmitsToNowhere)
{
    auto src = std::make_shared<PortNode>("src", 1, 1);
    auto dst = std::make_shared<PortNode>("dst", 1, 1);
    {
        Bus bus;
        bus.Register("src", src);
        bus.Register("dst", dst);
        TG_ASSERT(bus.Link("src", "dst", LinkOptions::Queued()));
    }
    TG_EXPECT(src->Out(0, Data()));
    TG_EXPECT_EQ(dst->received.load(), 0);
}

// ------------------------------------------------------------------ 命令参数 / Dump / 上下文
TG_TEST(BusCommandParams, TypedPayload)
{
    struct Rect
    {
        float x, y, w, h;
    };
    BusCommand cmd{BusCommandType::SetParam, 0, "overlay.rect", Rect{0.1f, 0.2f, 0.3f, 0.4f}};
    const Rect* r = cmd.Param<Rect>();
    TG_ASSERT(r != nullptr);
    TG_EXPECT(r->w > 0.29f && r->w < 0.31f);
    TG_EXPECT(cmd.Param<int>() == nullptr);   // 类型不符返回空，不抛

    BusCommand legacy{BusCommandType::SetBitrate, 2'000'000};
    TG_EXPECT_EQ(legacy.arg, int64_t{2'000'000});
    TG_EXPECT(!legacy.payload.has_value());
}

TG_TEST(BusDump, ListsNodesAndLinksSorted)
{
    Bus bus;
    bus.Register("b", std::make_shared<PortNode>("b", kDynamicPorts, 1));
    bus.Register("a", std::make_shared<PortNode>("a", 1, 2));
    TG_ASSERT(bus.Link("a", 1, "b", 3, LinkOptions::Queued(5)));
    const auto lines = bus.Dump();
    TG_ASSERT(lines.size() == 3);
    TG_EXPECT(lines[0].rfind("node a in=1 out=2", 0) == 0);
    TG_EXPECT(lines[1].rfind("node b in=*", 0) == 0);
    TG_EXPECT(lines[2].rfind("link a:1 -> b:3 queued[5]", 0) == 0);
}

TG_TEST(BusContext, ProvideAndGetByType)
{
    struct Clock
    {
        int64_t now = 42;
    };
    Bus bus;
    auto node = std::make_shared<PortNode>("n", 1, 1);
    TG_EXPECT(node->Ctx() == nullptr);
    bus.Register("n", node);
    TG_ASSERT(node->Ctx() != nullptr);
    TG_EXPECT(node->Ctx()->Get<Clock>() == nullptr);
    bus.Context().Provide(std::make_shared<Clock>());
    TG_ASSERT(node->Ctx()->Get<Clock>() != nullptr);
    TG_EXPECT_EQ(node->Ctx()->Get<Clock>()->now, int64_t{42});
    bus.Context().Provide<Clock>(nullptr);
    TG_EXPECT(node->Ctx()->Get<Clock>() == nullptr);
}

TG_TEST(BusFrame, ControlFrameHelpers)
{
    auto f = MakeControlFrame(ControlKind::EOS, MediaKind::Audio);
    TG_EXPECT(f->IsControl());
    TG_EXPECT(f->endOfStream);
    TG_EXPECT(f->streamKind == MediaKind::Audio);
    TG_EXPECT(!Data()->IsControl());
}

// ------------------------------------------------------------------ 生命周期 / 分组启停
namespace
{

struct Journal
{
    std::mutex mutex;
    std::vector<std::string> lines;
    void Add(std::string s)
    {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(std::move(s));
    }
    std::string Joined()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::string out;
        for (const auto& l : lines) {
            out += l + ",";
        }
        return out;
    }
};

class LifeNode : public BusNode
{
public:
    LifeNode(std::string name, NodeGroup group, Journal& j) : BusNode(std::move(name)), m_group(group), m_j(j) {}

    NodeGroup Group() const override { return m_group; }
    bool IsActive() const override { return active; }
    bool GatedOutput() const override { return gated; }
    int NodeConfigure() override
    {
        m_j.Add("cfg:" + Name());
        return configureRet;
    }
    bool NodeStart() override
    {
        m_j.Add("start:" + Name());
        return startOk;
    }
    void NodeStop() override { m_j.Add("stop:" + Name()); }
    void NodeClose() override { m_j.Add("close:" + Name()); }

    bool Submit(FramePtr frame) override { return Forward(std::move(frame)); }
    size_t Pending() const override { return 0; }
    bool Out(FramePtr f) { return Emit(0, std::move(f)); }

    bool active = true;
    bool gated = false;
    bool startOk = true;
    int configureRet = 0;

private:
    NodeGroup m_group;
    Journal& m_j;
};

// src(capture) -> enc(output) -> mux(output)，另一路 aenc(output) -> mux
struct LifeGraph
{
    Journal j;
    Bus bus;
    std::shared_ptr<LifeNode> src = std::make_shared<LifeNode>("src", NodeGroup::Capture, j);
    std::shared_ptr<LifeNode> venc = std::make_shared<LifeNode>("venc", NodeGroup::Output, j);
    std::shared_ptr<LifeNode> aenc = std::make_shared<LifeNode>("aenc", NodeGroup::Output, j);
    std::shared_ptr<LifeNode> mux = std::make_shared<LifeNode>("mux", NodeGroup::Output, j);
    std::shared_ptr<PortNode> sink = std::make_shared<PortNode>("sink", 1, 1);

    LifeGraph()
    {
        bus.Begin()
            .Register("src", src)
            .Register("venc", venc)
            .Register("aenc", aenc)
            .Register("mux", mux)
            .Register("sink", sink)
            .Link("src", "venc")
            .Link("venc", "mux")
            .Link("aenc", "mux")
            .Link("mux", "sink")
            .Commit();
    }
};

}  // namespace

TG_TEST(BusLifecycle, GroupOrderFollowsTopology)
{
    LifeGraph g;
    TG_EXPECT(g.bus.ConfigureGroup(NodeGroup::Output).ok);
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Output).ok);
    g.bus.StopGroup(NodeGroup::Output);
    g.bus.CloseGroup(NodeGroup::Output);
    TG_EXPECT_EQ(g.j.Joined(),
                 std::string("cfg:aenc,cfg:venc,cfg:mux,"        // 上游先于下游
                             "start:mux,start:venc,start:aenc,"  // 下游先于上游
                             "stop:aenc,stop:venc,stop:mux,"     // 上游先于下游
                             "close:mux,close:venc,close:aenc,"));   // 下游先于上游
    TG_EXPECT(g.src->Lifecycle() == NodeLifecycle::Created);   // capture 组不受影响
    TG_EXPECT(g.mux->Lifecycle() == NodeLifecycle::Closed);
}

TG_TEST(BusLifecycle, GroupsStartAndStopIndependently)
{
    LifeGraph g;
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Capture).ok);
    TG_EXPECT(g.src->Lifecycle() == NodeLifecycle::Running);
    TG_EXPECT(g.mux->Lifecycle() == NodeLifecycle::Created);
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Output).ok);
    g.bus.StopGroup(NodeGroup::Output);
    TG_EXPECT(g.src->Lifecycle() == NodeLifecycle::Running);   // 停播不停采集（预览继续）
    TG_EXPECT(g.mux->Lifecycle() == NodeLifecycle::Stopped);
    const auto dump = g.bus.DumpGraph();
    TG_EXPECT(dump.find("node src in=1 out=1 pending=0 group=capture state=running") != std::string::npos);
    TG_EXPECT(dump.find("node mux in=1 out=1 pending=0 group=output state=stopped") != std::string::npos);
}

TG_TEST(BusLifecycle, InactiveNodesAreSkippedOnStart)
{
    LifeGraph g;
    g.aenc->active = false;   // 纯视频模式
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Output).ok);
    TG_EXPECT(g.j.Joined().find("start:aenc") == std::string::npos);
    TG_EXPECT(g.aenc->Lifecycle() == NodeLifecycle::Created);
}

TG_TEST(BusLifecycle, StartGroupSkipsRunningNodes)
{
    // 预览期间已经起来的节点，开播再起一次组时不能重复启动（否则采集线程起两遍）
    LifeGraph g;
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Output).ok);
    const std::string once = g.j.Joined();
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Output).ok);
    TG_EXPECT_EQ(g.j.Joined(), once);
    g.bus.StopGroup(NodeGroup::Output);
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Output).ok);   // 停止后可以再起
    TG_EXPECT(g.venc->Lifecycle() == NodeLifecycle::Running);
}

TG_TEST(BusLifecycle, StartFailureRollsBackStartedNodes)
{
    LifeGraph g;
    g.venc->startOk = false;
    const auto r = g.bus.StartGroup(NodeGroup::Output);
    TG_EXPECT(!r.ok);
    TG_EXPECT(r.failedNode == "venc");
    TG_EXPECT_EQ(g.j.Joined(), std::string("start:mux,start:venc,stop:mux,"));
    TG_EXPECT(g.mux->Lifecycle() == NodeLifecycle::Stopped);
}

TG_TEST(BusLifecycle, ConfigureStopsAtFirstFailure)
{
    LifeGraph g;
    g.venc->configureRet = -5;
    const auto r = g.bus.ConfigureGroup(NodeGroup::Output);
    TG_EXPECT(!r.ok);
    TG_EXPECT(r.failedNode == "venc");
    TG_EXPECT_EQ(r.code, -5);
    TG_EXPECT(g.j.Joined().find("cfg:mux") == std::string::npos);
}

TG_TEST(BusLifecycle, GatedOutputBlocksEmitAfterStop)
{
    LifeGraph g;
    g.src->gated = true;
    g.venc->gated = false;
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Capture).ok);
    TG_EXPECT(g.src->Out(Data()));
    TG_EXPECT_EQ(g.sink->received.load(), 1);
    g.bus.StopGroup(NodeGroup::Capture);
    TG_EXPECT(!g.src->Out(Data()));   // 关闸后丢弃
    TG_EXPECT_EQ(g.sink->received.load(), 1);
    TG_EXPECT(g.venc->Out(Data()));   // 非受控节点不受影响（编码器停止时要 drain）
    TG_EXPECT(g.bus.StartGroup(NodeGroup::Capture).ok);
    TG_EXPECT(g.src->Out(Data()));    // 重新启动后开闸
}

TG_TEST(BusLifecycle, StressGatedStopWhileEmitting)
{
    LifeGraph g;
    g.src->gated = true;
    for (int round = 0; round < 50; ++round) {
        TG_ASSERT(g.bus.StartGroup(NodeGroup::Capture).ok);
        std::atomic<bool> stop{false};
        std::atomic<int> afterStop{0};
        std::atomic<bool> stopped{false};
        std::thread producer([&] {
            while (!stop.load()) {
                const bool wasStopped = stopped.load();   // 先读：读到 true 时 StopGroup 已返回
                const bool ok = g.src->Out(Data());
                if (ok && wasStopped) {
                    ++afterStop;   // StopGroup 返回之后仍放行 = 违反保证
                }
            }
        });
        std::this_thread::sleep_for(100us);
        g.bus.StopGroup(NodeGroup::Capture);
        stopped = true;
        std::this_thread::sleep_for(100us);
        stop = true;
        producer.join();
        TG_EXPECT_EQ(afterStop.load(), 0);
    }
}

TG_TEST(EmitGateScope, PairsEnterAndLeave)
{
    EmitGate gate;
    {
        EmitGate::Scope s(gate);
        TG_EXPECT(static_cast<bool>(s));
    }
    gate.Close();
    gate.WaitDrained();   // 计数已归零，不会卡住
    EmitGate::Scope s2(gate);
    TG_EXPECT(!static_cast<bool>(s2));
    TG_EXPECT(!gate.IsOpen());
}
