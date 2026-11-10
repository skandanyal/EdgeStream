#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>

#include "include/ringBuffer.hpp"
#include "include/database.hpp"
#include "include/detector.hpp"
#include "include/tracker.hpp"
#include "include/zoneMonitor.hpp"

void producer(RingBuffer<cv::Mat> &rb, const std::string &source) {
    cv::VideoCapture cap;
    if (source == "0") cap.open(0);
    else cap.open(source);

    if (!cap.isOpened()) {
        std::cerr << "[ERROR] Producer unable to open video source: " << source << "\n";
        rb.thread_finished();
        return;
    }

    double video_fps = cap.get(cv::CAP_PROP_FPS);
    if (video_fps <= 0.0 || video_fps > 60.0) video_fps = 30.0;
    auto frame_duration = std::chrono::milliseconds(static_cast<int>(1000.0 / video_fps));

    cv::Mat frame;
    while (!rb.is_stopped() && cap.read(frame)) {
        if (frame.empty()) break;
        if (!rb.push(frame, true)) break;

        // Pace the producer to match video framerate (only for video files, not device 0)
        if (source != "0") {
            std::this_thread::sleep_for(frame_duration);
        }
    }

    cap.release();
    rb.thread_finished();
}

void consumer(RingBuffer<cv::Mat> &rb, Database &db, const YAML::Node &config) {
    // 1. Initialize GPU Detector
    DetectorConfig det_cfg;
    det_cfg.model_path = config["model"]["path"] ? config["model"]["path"].as<std::string>() :
    "models/yolov8n.onnx";
    det_cfg.input_width = config["image_configs"]["image_width"].as<int>();
    det_cfg.input_height = config["image_configs"]["image_height"].as<int>();
    det_cfg.score_threshold = config["pipeline_configs"]["score_threshold"].as<float>();
    det_cfg.nms_threshold = config["pipeline_configs"]["nms_threshold"].as<float>();
    det_cfg.confidence_threshold = config["pipeline_configs"]["confidence_threshold"].as<float>();
    det_cfg.use_cuda = (config["model"]["backend"] && config["model"]["backend"].as<std::string>() == "CUDA");

    Detector detector(det_cfg);
    Tracker tracker;

    // 2. Initialize Zone Monitor
    ZoneConfig zone_cfg;
    zone_cfg.min_dwell_time_seconds = config["zone_configs"]["min_dwell_time_seconds"].as<float>();
    if (config["zone_configs"]["polygon"]) {
        for (const auto &pt : config["zone_configs"]["polygon"]) {
            zone_cfg.polygon_points.emplace_back(pt[0].as<int>(), pt[1].as<int>());
        }
    }
    ZoneMonitor zone_monitor(zone_cfg);

    // 3. Setup Scaled GUI Window (or Headless Mode for Docker / Servers)
    const std::string window_name = "EdgeStream Core Engine";
    bool is_headless = (std::getenv("DISPLAY") == nullptr);
    if (!is_headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 1280, 720);
    } else {
        std::cout << "[INFO] Running in HEADLESS mode (no GUI display attached).\n";
    }

    cv::Mat frame;
    int displayed_frames = 0;
    auto last_time = std::chrono::high_resolution_clock::now();

    while (rb.pop(frame)) {
        if (frame.empty()) continue;
        displayed_frames++;

        // Calculate instantaneous FPS
        auto curr_time = std::chrono::high_resolution_clock::now();
        auto elapsed_sec = std::chrono::duration<float>(curr_time - last_time).count();
        last_time = curr_time;

        float fps = 30.0f;
        if (displayed_frames > 1 && elapsed_sec > 0.001f) {
            fps = std::min(1.0f / elapsed_sec, 120.0f); // Clamp realistic frame rate
        }

        // Pipeline: Detect -> Track -> Zone Compliance
        auto detections = detector.detect(frame);
        auto tracks = tracker.update(detections);
        auto alerts = zone_monitor.update(tracks);

        // Telemetry logging into SQLite
        int current_queue_size = rb.get_size();
        int dropped_frames = static_cast<int>(rb.get_dropped_count());
        db.log_frame(displayed_frames, current_queue_size, fps, dropped_frames);

        // Rendering overlays
        zone_monitor.draw_zone(frame, alerts);

        for (const auto &trk : tracks) {
            bool is_flagged = false;
            float dwell = 0.0f;
            for (const auto &alert : alerts) {
                if (alert.track_id == trk.track_id) {
                    is_flagged = alert.is_flagged;
                    dwell = alert.dwell_time_seconds;
                    break;
                }
            }

            cv::Scalar box_color = is_flagged ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
            cv::rectangle(frame, trk.box, box_color, 2);

            std::string label = "ID:" + std::to_string(trk.track_id) + " " + trk.class_name;
            if (dwell > 0.0f) {
                label += is_flagged ? " [FLAGGED " + cv::format("%.1fs]", dwell)
                                    : " [" + cv::format("%.1fs]", dwell) + "]";
            }
            cv::putText(frame, label, cv::Point(trk.box.x, std::max(20, trk.box.y - 5)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, box_color, 2);
        }

        cv::putText(frame, cv::format("FPS: %.1f | Queue: %d | Dropped: %d", fps, current_queue_size,
        dropped_frames),
                    cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

        if (!is_headless) {
            cv::imshow(window_name, frame);

            char key = static_cast<char>(cv::waitKey(1));
            bool window_closed = cv::getWindowProperty(window_name, cv::WND_PROP_VISIBLE) < 1;
            if (key == 27 || key == 'q' || key == 'Q' || window_closed) {
                rb.stop();
                break;
            }
        } else {
            if (displayed_frames % 30 == 0) {
                std::cout << "[HEADLESS] Frame " << displayed_frames 
                          << " | FPS: " << cv::format("%.1f", fps)
                          << " | Queue: " << current_queue_size 
                          << " | Dropped: " << dropped_frames << "\r" << std::flush;
            }
        }
    }

    rb.stop();
    if (!is_headless) {
        cv::destroyAllWindows();
        cv::waitKey(1);
    }
}

int main(int argc, char *argv[]) {
    std::string source = (argc > 1) ? argv[1] : "assets/sample_video.mp4";

    try {
        YAML::Node config = YAML::LoadFile("config.yaml");

        std::string db_path = (config["database"] && config["database"]["path"])
                                  ? config["database"]["path"].as<std::string>()
                                  : "database/EdgeStream_data.db";

        // Read buffer parameters dynamically from config.yaml at runtime
        size_t capacity = (config["buffer_configs"] && config["buffer_configs"]["capacity"])
                              ? config["buffer_configs"]["capacity"].as<size_t>() : 50;
        int num_producers = (config["buffer_configs"] && config["buffer_configs"]["num_producers"])
                                ? config["buffer_configs"]["num_producers"].as<int>() : 1;

        RingBuffer<cv::Mat> rb(capacity, num_producers);

        auto start_time = std::chrono::high_resolution_clock::now();
        {
            Database db(db_path);

            // Producer runs asynchronously on background thread
            std::thread prod(producer, std::ref(rb), std::cref(source));

            // Consumer runs on main thread (handles GPU inference and GUI event loop)
            consumer(std::ref(rb), std::ref(db), config);

            prod.join();

            auto end_time = std::chrono::high_resolution_clock::now();
            auto exit_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

            std::cout << "\n================ Final Pipeline Telemetry ================\n";
            db.display_telemetry_metrics();
            std::cout << "Thread Exit Latency: " << exit_latency_us << " us\n";
            std::cout << "==========================================================\n";
        } // Database is closed cleanly here via RAII destructor

        // Delete the database file so every run starts with clean, isolated metrics
        std::error_code ec;
        if (std::filesystem::remove(db_path, ec)) {
            std::cout << "[INFO] Telemetry database reset (" << db_path << " removed).\n";
        }

    } catch (const std::exception &e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
