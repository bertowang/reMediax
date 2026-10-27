#include <chrono>
#include <thread>

#include "core/Queue.h"
#include "tg_test.h"

using namespace TGCapture::core;
using namespace std::chrono_literals;

TG_TEST(Queue, DropOldestKeepsNewestAndCounts)
{
    BoundedQueue<int> q(3, DropPolicy::DropOldest);
    for (int i = 1; i <= 5; ++i) {
        q.Push(i);
    }
    TG_EXPECT_EQ(q.Size(), size_t{3});
    TG_EXPECT_EQ(q.DroppedCount(), uint64_t{2});
    TG_EXPECT_EQ(*q.TryPop(10ms), 3);
}

TG_TEST(Queue, DropNewestKeepsOldest)
{
    BoundedQueue<int> q(2, DropPolicy::DropNewest);
    q.Push(1);
    q.Push(2);
    q.Push(3);
    TG_EXPECT_EQ(q.DroppedCount(), uint64_t{1});
    TG_EXPECT_EQ(*q.TryPop(10ms), 1);
}

TG_TEST(Queue, BlockPolicyBlocksUntilConsumerPops)
{
    BoundedQueue<int> q(1, DropPolicy::Block);
    q.Push(1);
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        q.Push(2);
        pushed = true;
    });
    std::this_thread::sleep_for(80ms);
    TG_EXPECT(!pushed.load());
    TG_EXPECT_EQ(*q.TryPop(10ms), 1);
    producer.join();
    TG_EXPECT(pushed.load());
}

TG_TEST(Queue, CloseUnblocksBlockedProducerAndConsumer)
{
    BoundedQueue<int> full(1, DropPolicy::Block);
    full.Push(1);
    std::thread producer([&] { TG_EXPECT(!full.Push(2)); });
    BoundedQueue<int> empty(1);
    std::thread consumer([&] { TG_EXPECT(!empty.Pop().has_value()); });
    std::this_thread::sleep_for(50ms);
    full.Close();
    empty.Close();
    producer.join();
    consumer.join();
}

TG_TEST(Queue, KeepProtectedDropsNonKeyFirst)
{
    BoundedQueue<int> q(3);
    auto isKey = [](int v) { return v % 10 == 0; };   // 10 / 20 视为关键帧
    q.PushKeepProtected(10, isKey);
    q.PushKeepProtected(1, isKey);
    q.PushKeepProtected(2, isKey);
    q.PushKeepProtected(3, isKey);   // 满：应丢 1（最旧的非关键），保留 10
    TG_EXPECT_EQ(q.DroppedCount(), uint64_t{1});
    TG_EXPECT_EQ(*q.TryPop(10ms), 10);
    TG_EXPECT_EQ(*q.TryPop(10ms), 2);
    TG_EXPECT_EQ(*q.TryPop(10ms), 3);
}

TG_TEST(Queue, KeepProtectedAllProtectedStillMakesRoom)
{
    BoundedQueue<int> q(2);
    auto allKey = [](int) { return true; };
    q.PushKeepProtected(10, allKey);
    q.PushKeepProtected(20, allKey);
    q.PushKeepProtected(30, allKey);   // 全是受保护的：只能丢最旧，否则永久塞满
    TG_EXPECT_EQ(q.Size(), size_t{2});
    TG_EXPECT_EQ(*q.TryPop(10ms), 20);
}

TG_TEST(Queue, ReopenClearsAndResets)
{
    BoundedQueue<int> q(2);
    q.Push(1);
    q.Push(2);
    q.Push(3);
    q.Close();
    TG_EXPECT(!q.Push(4));
    q.Reopen();
    TG_EXPECT_EQ(q.Size(), size_t{0});
    TG_EXPECT_EQ(q.DroppedCount(), uint64_t{0});
    TG_EXPECT(q.Push(5));
}

TG_TEST(Queue, DroppedCountReadableConcurrentlyWithProducer)
{
    BoundedQueue<int> q(4);
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        uint64_t last = 0;
        while (!stop.load()) {
            const uint64_t now = q.DroppedCount();
            TG_EXPECT(now >= last);   // 单调不减
            last = now;
        }
    });
    for (int i = 0; i < 20000; ++i) {
        q.Push(i);
    }
    stop = true;
    reader.join();
    TG_EXPECT_EQ(q.DroppedCount(), uint64_t{20000 - 4});
}
