#pragma once

#include "tracker.hpp"
#include <chrono>
#include <unordered_map>
#include <vector>

struct ZoneAlert {
    int track_id{-1};
    std::string class_name{};
    float dwell_time_seconds{};
    bool is_flagged{false}; // True if object exceeded minimum dwell time
};

struct ZoneConfig {
    float min_dwell_time_seconds{3.0f}; // Default 3.0 seconds, configurable
    std::vector<cv::Point> polygon_points{}; // Polygon zone boundary vertices
};

class ZoneMonitor {
public:
    explicit ZoneMonitor(ZoneConfig config = ZoneConfig{});
    ~ZoneMonitor() = default;

    // Checks tracks against the zone, returns alerts for all objects currently in zone
    std::vector<ZoneAlert> update(const std::vector<TrackedObject>& tracks);

    // Draws polygon and violation alerts onto the frame
    void draw_zone(cv::Mat& frame, const std::vector<ZoneAlert>& alerts) const;

    void set_min_dwell_time(float seconds) { config_.min_dwell_time_seconds = seconds; }
    void set_polygon(const std::vector<cv::Point>& pts) { config_.polygon_points = pts; }

private:
    ZoneConfig config_;
    // track_id -> timestamp when object was first detected inside polygon
    std::unordered_map<int, std::chrono::steady_clock::time_point> entry_timestamps_;
};