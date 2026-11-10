#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

struct Detection {
    int class_id{0};
    std::string class_name{};
    float confidence{0.0f};
    cv::Rect box{}; // [left, top, width, height] in original image coordinates
};

struct DetectorConfig {
    std::string model_path{"models/yolov8n.onnx"};
    int input_width{640};
    int input_height{640};
    float score_threshold{0.45f};
    float nms_threshold{0.50f};
    float confidence_threshold{0.50f};
    bool use_cuda{true};
};

class Detector {
public:
    explicit Detector(const DetectorConfig& config);
    ~Detector() = default;

    [[nodiscard]] std::vector<Detection> detect(const cv::Mat& frame);
    static const std::vector<std::string>& get_coco_labels();

private:
    DetectorConfig config_;
    std::vector<std::string> labels_;

    cv::dnn::Net net_;

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "EdgeStream_YOLOv8"};
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> ort_session_;
};
