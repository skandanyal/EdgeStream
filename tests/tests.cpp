#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio> // for std::remove

#include "../include/ringBuffer.hpp"
#include "../include/database.hpp"


TEST(RingBufferTest, InitialStateIsEmpty) {
    constexpr int CAPACITY = 5;
    constexpr int NUM_PRODS = 1;
    RingBuffer<int, CAPACITY, NUM_PRODS> rb;

    EXPECT_EQ(rb.get_size(), 0);
    EXPECT_FALSE(rb.is_stopped());
}


TEST(RingBufferTest, MaintainsStrictFIFOOrder) {
    constexpr int CAPACITY = 5;
    constexpr int NUM_PRODS = 1;
    RingBuffer<int, CAPACITY, NUM_PRODS> rb;

    // Push 5 items
    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(rb.push(i));
    }
    EXPECT_EQ(rb.get_size(), 5);

    // Pop and verify exact sequence (1, 2, 3, 4, 5)
    for (int i = 1; i <= 5; ++i) {
        int val = 0;
        ASSERT_TRUE(rb.pop(val));
        EXPECT_EQ(val, i);
    }

    EXPECT_EQ(rb.get_size(), 0);
}


TEST(RingBufferTest, PopReturnsFalseWhenEmptyAndProducersFinished) {
    constexpr int CAPACITY = 5;
    constexpr int NUM_PRODS = 1;
    RingBuffer<int, CAPACITY, NUM_PRODS> rb;

    int dummy = 42;
    rb.push(dummy);

    int out_val = 0;
    EXPECT_TRUE(rb.pop(out_val));
    EXPECT_EQ(out_val, 42);

    // Signal producer finished
    rb.thread_finished();

    // Next pop on empty buffer must return false
    EXPECT_FALSE(rb.pop(out_val));
}


TEST(RingBufferConcurrencyTest, StressTestNoDataLossAcrossProducers) {
    constexpr int CAPACITY = 10;
    constexpr int NUM_PRODS = 2;
    constexpr int ITEMS_PER_PROD = 500;
    constexpr int TOTAL_ITEMS = NUM_PRODS * ITEMS_PER_PROD; // 1,000 items

    RingBuffer<int, CAPACITY, NUM_PRODS> rb;
    std::atomic<int> consumed_count = 0;

    auto producer_worker = [&](int id) {
        for (int i = 0; i < ITEMS_PER_PROD; ++i) {
            int val = id * 1000 + i;
            rb.push(val);
        }
        rb.thread_finished();
    };

    auto consumer_worker = [&]() {
        int val = 0;
        while (rb.pop(val)) {
            consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Spawn 2 producers and 1 consumer
    std::thread p1(producer_worker, 1);
    std::thread p2(producer_worker, 2);
    std::thread c1(consumer_worker);

    p1.join();
    p2.join();
    c1.join();

    // Verify all 1,000 items were safely passed through the queue
    EXPECT_EQ(consumed_count.load(), TOTAL_ITEMS);
    EXPECT_EQ(rb.get_size(), 0);
}


TEST(RingBufferConcurrencyTest, GracefulShutdownUnblocksConsumer) {
    constexpr int CAPACITY = 10;
    constexpr int NUM_PRODS = 1;
    RingBuffer<int, CAPACITY, NUM_PRODS> rb;

    std::atomic<bool> consumer_woke_up = false;

    // Consumer blocks waiting for items on an empty buffer
    std::thread cons([&]() {
        int val = 0;
        bool result = rb.pop(val); // Blocks on cv.wait!
        EXPECT_FALSE(result);      // Must return false when stopped
        consumer_woke_up = true;
    });

    // Give consumer time to enter cv.wait()
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Call stop()
    auto start_time = std::chrono::steady_clock::now();
    rb.stop();

    cons.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    // Verify consumer woke up cleanly in less than 100ms
    EXPECT_TRUE(consumer_woke_up.load());
    EXPECT_LT(elapsed, 100);
}


class DatabaseTest : public ::testing::Test {
protected:
    const std::string test_db_name = "test_gtest_edgestream.db";

    void SetUp() override {
        std::remove(test_db_name.c_str());
    }

    void TearDown() override {
        std::remove(test_db_name.c_str());
    }
};


TEST_F(DatabaseTest, TableCreationAndInsertReturnsSuccess) {
    Database db(test_db_name);

    long long now_ms = 1730000000000;
    int rc = db.insert_telemetry(1, now_ms, 5, 29.97f);
    EXPECT_EQ(rc, 0);

    rc = db.insert_telemetry(2, now_ms + 33, 6, 30.10f);
    EXPECT_EQ(rc, 0);
}


TEST_F(DatabaseTest, AggregateMetricsCalculationRunsCleanly) {
    Database db(test_db_name);

    for (int frame = 1; frame <= 10; ++frame) {
        long long timestamp = 1730000000000 + frame * 33;
        int queue_size = frame % 4;
        float fps = 30.0f;
        EXPECT_EQ(db.insert_telemetry(frame, timestamp, queue_size, fps), 0);
    }

    // Must execute aggregate query without SQL syntax or runtime errors
    int rc = db.display_telemetry_metrics();
    EXPECT_EQ(rc, 0);
}
