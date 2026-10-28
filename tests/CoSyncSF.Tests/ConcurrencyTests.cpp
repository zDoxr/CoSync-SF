#include "cosync/shared/Concurrency.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace cosync;

TEST(BoundedQueue, EnforcesCapacityAndOrder)
{
    BoundedQueue<int> q(3);
    EXPECT_TRUE(q.TryPush(1));
    EXPECT_TRUE(q.TryPush(2));
    EXPECT_TRUE(q.TryPush(3));
    EXPECT_FALSE(q.TryPush(4));
    EXPECT_EQ(q.Size(), 3u);

    std::vector<int> out;
    EXPECT_EQ(q.Drain(out, 2), 2u);
    EXPECT_EQ(out, (std::vector<int>{ 1, 2 }));
    EXPECT_TRUE(q.TryPush(5));
    q.Drain(out, 10);
    EXPECT_EQ(out, (std::vector<int>{ 1, 2, 3, 5 }));
}

TEST(BoundedQueue, ConcurrentProducersNeverExceedCapacity)
{
    BoundedQueue<int> q(1000);
    std::atomic<int> accepted{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (q.TryPush(i)) ++accepted;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(accepted.load(), 1000);
    EXPECT_EQ(q.Size(), 1000u);
}

TEST(LatestValueMailbox, KeepsNewestPerKey)
{
    LatestValueMailbox<int, std::string> box;
    box.Put(1, "a1");
    box.Put(2, "b1");
    box.Put(1, "a2");
    std::vector<std::string> out;
    box.TakeAll(out);
    std::ranges::sort(out);
    EXPECT_EQ(out, (std::vector<std::string>{ "a2", "b1" }));
    out.clear();
    box.TakeAll(out);
    EXPECT_TRUE(out.empty());

    box.Put(3, "c");
    box.Erase(3);
    box.TakeAll(out);
    EXPECT_TRUE(out.empty());
}
