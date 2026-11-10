#pragma once

#include "detector.hpp"
#include <vector>
#include <unordered_map>

struct TrackedObject {
    int track_id{-1};
    int class_id{0};
    std::string class_name{};
    float confidence{0.0f};
    cv::Rect box{};
    int lost_frames{0};
};

struct TrackerConfig {
    float iou_threshold{0.30f}; // Minimum IoU overlap to match a detection to an existing track
    int max_lost_frames{15};     // Frames to keep a track alive if temporarily occluded / missed
};

class Tracker {
public:
    explicit Tracker(TrackerConfig config = TrackerConfig{});
    ~Tracker() = default;

    // Ingests new detections from current frame and returns updated active tracks
    std::vector<TrackedObject> update(const std::vector<Detection>& detections);

    // Clears all internal tracking state
    void reset();

    // Utility: Computes Intersection over Union between two bounding boxes
    static float compute_iou(const cv::Rect& a, const cv::Rect& b);

private:
    TrackerConfig config_;
    int next_track_id_{1};
    std::vector<TrackedObject> active_tracks_;
};
