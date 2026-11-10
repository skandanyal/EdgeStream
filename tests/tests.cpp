#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <opencv2/opencv.hpp>

#include "../include/ringBuffer.hpp"
#include "../include/database.hpp"
#include "../include/detector.hpp"
#include "../include/tracker.hpp"
#include "../include/zoneMonitor.hpp"

// =========================================================================
// 1. RingBuffer Unit Tests (Runtime Dynamic Capacity & Eviction)
// =========================================================================

TEST(RingBufferTest, InitialStateIsEmpty) {
    RingBuffer<int> rb(5, 1);
    EXPECT_EQ(rb.get_size(), 0);
    EXPECT_EQ(rb.get_capacity(), 5);
    EXPECT_FALSE(rb.is_stopped());
    EXPECT_EQ(rb.get_dropped_count(), 0);
}

TEST(RingBufferTest, MaintainsStrictFIFOOrder) {
    RingBuffer<int> rb(5, 1);

    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(rb.push(i, false));
    }
    EXPECT_EQ(rb.get_size(), 5);

    for (int i = 1; i <= 5; ++i) {
        int val = 0;
        ASSERT_TRUE(rb.pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_EQ(rb.get_size(), 0);
}

TEST(RingBufferTest, DropOldestEvictionPolicyUnderOverflow) {
    // Capacity = 3
    RingBuffer<int> rb(3, 1);

    int a = 10, b = 20, c = 30;
    EXPECT_TRUE(rb.push(a, true));
    EXPECT_TRUE(rb.push(b, true));
    EXPECT_TRUE(rb.push(c, true));
    EXPECT_EQ(rb.get_size(), 3);
    EXPECT_EQ(rb.get_dropped_count(), 0);

    // Push 4th item with drop_oldest = true -> should drop 10 (oldest)
    int d = 40;
    EXPECT_TRUE(rb.push(d, true));
    EXPECT_EQ(rb.get_size(), 3);
    EXPECT_EQ(rb.get_dropped_count(), 1);

    // The surviving items must be 20, 30, 40
    int out = 0;
    ASSERT_TRUE(rb.pop(out)); EXPECT_EQ(out, 20);
    ASSERT_TRUE(rb.pop(out)); EXPECT_EQ(out, 30);
    ASSERT_TRUE(rb.pop(out)); EXPECT_EQ(out, 40);
    EXPECT_EQ(rb.get_size(), 0);
}

TEST(RingBufferTest, PopReturnsFalseWhenEmptyAndProducersFinished) {
    RingBuffer<int> rb(5, 1);
    int val = 42;
    rb.push(val, false);

    int out_val = 0;
    EXPECT_TRUE(rb.pop(out_val));
    EXPECT_EQ(out_val, 42);

    rb.thread_finished();
    EXPECT_FALSE(rb.pop(out_val));
}

TEST(RingBufferConcurrencyTest, StressTestNoDataLossAcrossProducers) {
    constexpr int CAPACITY = 10;
    constexpr int NUM_PRODS = 2;
    constexpr int ITEMS_PER_PROD = 500;
    constexpr int TOTAL_ITEMS = NUM_PRODS * ITEMS_PER_PROD;

    RingBuffer<int> rb(CAPACITY, NUM_PRODS);
    std::atomic<int> consumed_count = 0;

    auto producer_worker = [&](int id) {
        for (int i = 0; i < ITEMS_PER_PROD; ++i) {
            int val = id * 1000 + i;
            rb.push(val, false); // lossless blocking push
        }
        rb.thread_finished();
    };

    auto consumer_worker = [&]() {
        int val = 0;
        while (rb.pop(val)) {
            consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread p1(producer_worker, 1);
    std::thread p2(producer_worker, 2);
    std::thread c1(consumer_worker);

    p1.join();
    p2.join();
    c1.join();

    EXPECT_EQ(consumed_count.load(), TOTAL_ITEMS);
    EXPECT_EQ(rb.get_size(), 0);
    EXPECT_EQ(rb.get_dropped_count(), 0);
}

TEST(RingBufferConcurrencyTest, GracefulShutdownUnblocksConsumer) {
    RingBuffer<int> rb(10, 1);
    std::atomic<bool> consumer_woke_up = false;

    std::thread cons([&]() {
        int val = 0;
        bool result = rb.pop(val);
        EXPECT_FALSE(result);
        consumer_woke_up = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto start_time = std::chrono::steady_clock::now();
    rb.stop();

    cons.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    EXPECT_TRUE(consumer_woke_up.load());
    EXPECT_LT(elapsed, 100);
}

// Helper to locate test assets regardless of working directory
inline std::string resolve_test_path(const std::string& rel_path) {
#ifdef PROJECT_SOURCE_DIR
    std::filesystem::path proj_dir = PROJECT_SOURCE_DIR;
    auto abs_candidate = proj_dir / rel_path;
    if (std::filesystem::exists(abs_candidate)) {
        return abs_candidate.string();
    }
#endif
    if (std::filesystem::exists(rel_path)) return rel_path;
    if (std::filesystem::exists("../" + rel_path)) return "../" + rel_path;
    if (std::filesystem::exists("../../" + rel_path)) return "../../" + rel_path;
    return rel_path;
}

// =========================================================================
// 2. Detector Systemic Tests (Offline Static Image Verification)
// =========================================================================

TEST(DetectorTest, DetectsObjectsOnSampleImage) {
    DetectorConfig cfg;
    cfg.model_path = resolve_test_path("models/yolov8n.onnx");
    cfg.input_width = 640;
    cfg.input_height = 640;
    cfg.score_threshold = 0.25f;
    cfg.nms_threshold = 0.45f;
    cfg.confidence_threshold = 0.25f;
    cfg.use_cuda = false; // CPU execution ensures test runs in any environment

    ASSERT_TRUE(std::filesystem::exists(cfg.model_path)) << "Model file not found: " << cfg.model_path;

    Detector detector(cfg);

    std::string img_path = resolve_test_path("assets/cats2.jpeg");
    cv::Mat img = cv::imread(img_path);
    ASSERT_FALSE(img.empty()) << "Test asset not found: " << img_path;

    auto detections = detector.detect(img);

    // Expect at least 1 cat detected
    EXPECT_GE(detections.size(), 1u);
    for (const auto& det : detections) {
        EXPECT_GT(det.confidence, 0.25f);
        EXPECT_GT(det.box.width, 0);
        EXPECT_GT(det.box.height, 0);
    }
}

// =========================================================================
// 3. Multi-Object Tracker Tests (IoU Association & Track Lifecycle)
// =========================================================================

TEST(TrackerTest, GeneratesAndMaintainsStickyTrackIDs) {
    Tracker tracker;

    // Frame 1: Detection at (100, 100)
    std::vector<Detection> frame1_dets = {
        {15, "cat", 0.90f, cv::Rect(100, 100, 60, 60)}
    };
    auto tracks_f1 = tracker.update(frame1_dets);
    ASSERT_EQ(tracks_f1.size(), 1u);
    int assigned_id = tracks_f1[0].track_id;
    EXPECT_GT(assigned_id, 0);

    // Frame 2: Object moves slightly to (104, 102) -> IoU overlap is high
    std::vector<Detection> frame2_dets = {
        {15, "cat", 0.88f, cv::Rect(104, 102, 60, 60)}
    };
    auto tracks_f2 = tracker.update(frame2_dets);
    ASSERT_EQ(tracks_f2.size(), 1u);
    EXPECT_EQ(tracks_f2[0].track_id, assigned_id) << "Track ID must persist across consecutive overlapping frames.";

    // Frame 3: A new distinct object appears at (400, 400)
    std::vector<Detection> frame3_dets = {
        {15, "cat", 0.85f, cv::Rect(108, 104, 60, 60)},
        {2, "car", 0.92f, cv::Rect(400, 400, 100, 80)}
    };
    auto tracks_f3 = tracker.update(frame3_dets);
    ASSERT_EQ(tracks_f3.size(), 2u);
    EXPECT_EQ(tracks_f3[0].track_id, assigned_id);
    EXPECT_NE(tracks_f3[1].track_id, assigned_id);
}

TEST(TrackerTest, PurgesLostTracksAfterMaxLostFrames) {
    TrackerConfig cfg;
    cfg.max_lost_frames = 3;
    Tracker tracker(cfg);

    // Register track
    std::vector<Detection> dets = {
        {15, "cat", 0.90f, cv::Rect(50, 50, 40, 40)}
    };
    auto tracks = tracker.update(dets);
    ASSERT_EQ(tracks.size(), 1u);

    // Simulate 4 consecutive empty frames where detection is lost
    for (int i = 0; i < 4; ++i) {
        tracker.update({});
    }

    // Now re-introduce an object far away: old track must be dead, new ID assigned
    std::vector<Detection> new_dets = {
        {15, "cat", 0.90f, cv::Rect(300, 300, 40, 40)}
    };
    auto new_tracks = tracker.update(new_dets);
    ASSERT_EQ(new_tracks.size(), 1u);
    EXPECT_NE(new_tracks[0].track_id, tracks[0].track_id) << "Dead track must be purged, spawning a new ID.";
}

// =========================================================================
// 4. Polygon Zone Compliance & Loitering Tests
// =========================================================================

TEST(ZoneMonitorTest, FlagsLoiteringViolationAfterDwellTime) {
    ZoneConfig cfg;
    cfg.min_dwell_time_seconds = 0.05f; // 50 milliseconds for rapid testing
    cfg.polygon_points = {
        cv::Point(0, 0),
        cv::Point(200, 0),
        cv::Point(200, 200),
        cv::Point(0, 200)
    };
    ZoneMonitor monitor(cfg);

    // Track 1: inside zone (footpoint at x=50, y=100)
    TrackedObject trk1{1, 15, "cat", 0.9f, cv::Rect(20, 20, 60, 80), 0};

    // Track 2: outside zone (footpoint at x=350, y=380)
    TrackedObject trk2{2, 15, "cat", 0.9f, cv::Rect(300, 300, 100, 80), 0};

    // Immediate check: inside zone, but dwell time is ~0s -> NOT flagged yet
    auto alerts_t0 = monitor.update({trk1, trk2});
    ASSERT_EQ(alerts_t0.size(), 1u);
    EXPECT_EQ(alerts_t0[0].track_id, 1);
    EXPECT_FALSE(alerts_t0[0].is_flagged);

    // Sleep 60ms to exceed the 50ms dwell-time threshold
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Second check: dwell time > 0.05s -> MUST be flagged as violation
    auto alerts_t1 = monitor.update({trk1, trk2});
    ASSERT_EQ(alerts_t1.size(), 1u);
    EXPECT_EQ(alerts_t1[0].track_id, 1);
    EXPECT_TRUE(alerts_t1[0].is_flagged);
    EXPECT_GE(alerts_t1[0].dwell_time_seconds, 0.05f);
}

// =========================================================================
// 5. Database Telemetry Tests
// =========================================================================

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
    int rc = db.insert_telemetry(1, now_ms, 5, 29.97f, 0);
    EXPECT_EQ(rc, 0);

    rc = db.insert_telemetry(2, now_ms + 33, 6, 30.10f, 2);
    EXPECT_EQ(rc, 0);

    // Test streamlined log_frame helper
    rc = db.log_frame(3, 8, 31.5f, 5);
    EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, AggregateMetricsCalculationRunsCleanly) {
    Database db(test_db_name);

    for (int frame = 1; frame <= 10; ++frame) {
        long long timestamp = 1730000000000 + frame * 33;
        int queue_size = frame % 4;
        float fps = 30.0f;
        int dropped = frame / 3;
        EXPECT_EQ(db.insert_telemetry(frame, timestamp, queue_size, fps, dropped), 0);
    }

    int rc = db.display_telemetry_metrics();
    EXPECT_EQ(rc, 0);
}
