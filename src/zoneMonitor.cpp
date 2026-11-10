#include "../include/zoneMonitor.hpp"
#include <unordered_set>

ZoneMonitor::ZoneMonitor(ZoneConfig config)
    : config_(std::move(config)) {}

std::vector<ZoneAlert> ZoneMonitor::update(const std::vector<TrackedObject>& tracks) {
    auto now = std::chrono::steady_clock::now();
    std::vector<ZoneAlert> active_alerts;
    std::unordered_set<int> present_in_zone_ids;

    if (config_.polygon_points.size() < 3) {
        return active_alerts; // Need at least 3 points for a valid polygon
    }

    for (const auto& track : tracks) {
        // Use bottom-center point of bounding box ("foot-point" of object)
        cv::Point2f foot_point(
            static_cast<float>(track.box.x + track.box.width / 2.0f),
            static_cast<float>(track.box.y + track.box.height)
        );

        // cv::pointPolygonTest: returns >= 0 if inside or on edge
        double dist = cv::pointPolygonTest(config_.polygon_points, foot_point, false);

        if (dist >= 0.0) {
            present_in_zone_ids.insert(track.track_id);

            // Record entry timestamp if newly entered
            if (entry_timestamps_.find(track.track_id) == entry_timestamps_.end()) {
                entry_timestamps_[track.track_id] = now;
            }

            auto elapsed_sec = std::chrono::duration<float>(
                now - entry_timestamps_[track.track_id]
            ).count();

            ZoneAlert alert;
            alert.track_id = track.track_id;
            alert.class_name = track.class_name;
            alert.dwell_time_seconds = elapsed_sec;
            alert.is_flagged = (elapsed_sec >= config_.min_dwell_time_seconds);

            active_alerts.push_back(alert);
        }
    }

    // Clean up tracks that left the polygon or disappeared
    for (auto it = entry_timestamps_.begin(); it != entry_timestamps_.end(); ) {
        if (present_in_zone_ids.find(it->first) == present_in_zone_ids.end()) {
            it = entry_timestamps_.erase(it);
        } else {
            ++it;
        }
    }

    return active_alerts;
}

void ZoneMonitor::draw_zone(cv::Mat& frame, const std::vector<ZoneAlert>& alerts) const {
    if (config_.polygon_points.size() < 3) return;

    // Determine if any object in zone is currently FLAGGED
    bool violation = false;
    for (const auto& alert : alerts) {
        if (alert.is_flagged) {
            violation = true;
            break;
        }
    }

    // Green if compliant, Red if loitering violation
    cv::Scalar zone_color = violation ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);

    const cv::Point* pts_ptr = config_.polygon_points.data();
    int npts = static_cast<int>(config_.polygon_points.size());
    cv::polylines(frame, &pts_ptr, &npts, 1, true, zone_color, 2);

    // Overlay zone banner
    std::string banner = violation ? "[ALERT] RESTRICTED ZONE VIOLATION!" : "RESTRICTED ZONE (SAFE)";
    cv::putText(frame, banner, cv::Point(config_.polygon_points[0].x, std::max(25, config_.polygon_points[0].y - 10)),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, zone_color, 2);
}
