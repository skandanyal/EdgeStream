#include "../include/tracker.hpp"
#include <algorithm>

Tracker::Tracker(TrackerConfig config)
    : config_(config), next_track_id_(1) {}

float Tracker::compute_iou(const cv::Rect& a, const cv::Rect& b) {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);

    int intersection_w = std::max(0, x2 - x1);
    int intersection_h = std::max(0, y2 - y1);
    float intersection_area = static_cast<float>(intersection_w * intersection_h);

    float union_area = static_cast<float>(a.area() + b.area()) - intersection_area;
    if (union_area <= 0.0f) {
        return 0.0f;
    }
    return intersection_area / union_area;
}

std::vector<TrackedObject> Tracker::update(const std::vector<Detection>& detections) {
    std::vector<bool> detection_matched(detections.size(), false);
    std::vector<bool> track_matched(active_tracks_.size(), false);

    // 1. Match detections to active tracks using Greedy Highest-IoU matching
    for (size_t t = 0; t < active_tracks_.size(); ++t) {
        float best_iou = config_.iou_threshold;
        int best_det_idx = -1;

        for (size_t d = 0; d < detections.size(); ++d) {
            if (detection_matched[d]) continue;

            // Only match detections of the same semantic class
            if (active_tracks_[t].class_id != detections[d].class_id) continue;

            float iou = compute_iou(active_tracks_[t].box, detections[d].box);
            if (iou > best_iou) {
                best_iou = iou;
                best_det_idx = static_cast<int>(d);
            }
        }

        if (best_det_idx != -1) {
            // Update matched track
            active_tracks_[t].box = detections[best_det_idx].box;
            active_tracks_[t].confidence = detections[best_det_idx].confidence;
            active_tracks_[t].lost_frames = 0;
            track_matched[t] = true;
            detection_matched[best_det_idx] = true;
        } else {
            // Mark track as unobserved in this frame
            active_tracks_[t].lost_frames++;
        }
    }

    // 2. Remove dead tracks that exceeded max_lost_frames
    active_tracks_.erase(
        std::remove_if(active_tracks_.begin(), active_tracks_.end(),
                       [this](const TrackedObject& track) {
                           return track.lost_frames > config_.max_lost_frames;
                       }),
        active_tracks_.end());

    // 3. Register unassigned detections as new persistent tracks
    for (size_t d = 0; d < detections.size(); ++d) {
        if (!detection_matched[d]) {
            TrackedObject new_track;
            new_track.track_id = next_track_id_++;
            new_track.class_id = detections[d].class_id;
            new_track.class_name = detections[d].class_name;
            new_track.confidence = detections[d].confidence;
            new_track.box = detections[d].box;
            new_track.lost_frames = 0;
            active_tracks_.push_back(new_track);
        }
    }

    // Return current active tracks that are currently visible
    std::vector<TrackedObject> visible_tracks;
    for (const auto& track : active_tracks_) {
        if (track.lost_frames == 0) {
            visible_tracks.push_back(track);
        }
    }
    return visible_tracks;
}

void Tracker::reset() {
    active_tracks_.clear();
    next_track_id_ = 1;
}