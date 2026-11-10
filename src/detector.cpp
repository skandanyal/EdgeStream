#include "../include/detector.hpp"
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <array>

const std::vector<std::string>& Detector::get_coco_labels() {
    static const std::vector<std::string> coco_labels = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
        "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
        "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
        "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
        "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
        "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
        "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
        "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator",
        "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
    };
    return coco_labels;
}

Detector::Detector(const DetectorConfig& config)
    : config_(config), labels_(get_coco_labels()) {

    if (!std::filesystem::exists(config_.model_path)) {
        throw std::runtime_error("Detector Error: ONNX model file not found at " + config_.model_path);
    }

    net_ = cv::dnn::readNetFromONNX(config_.model_path);

    if (config_.use_cuda) {
        try {
            // Configure ONNX Runtime with NVIDIA CUDA Execution Provider
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = 0;
            session_options_.AppendExecutionProvider_CUDA(cuda_options);
            session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            ort_session_ = std::make_unique<Ort::Session>(env_, config_.model_path.c_str(), session_options_);
            std::cout << "[INFO] Detector initialized with NVIDIA CUDA ONNX Runtime backend." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[WARN] Failed to initialize CUDA provider (" << e.what() << "). Falling back to CPU.\n";
            config_.use_cuda = false;
        }

    } else {
        net_ = cv::dnn::readNetFromONNX(config_.model_path);
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        std::cout << "[INFO] Detector initialized with OpenCV CPU backend." << std::endl;
    }
}

std::vector<Detection> Detector::detect(const cv::Mat& frame) {
    if (frame.empty()) return {};

    // 1. Preprocess image to NCHW 1x3x640x640 float blob scaled [0, 1]
    cv::Mat blob;
    cv::dnn::blobFromImage(frame, blob, 1.0 / 255.0,
                           cv::Size(config_.input_width, config_.input_height),
                           cv::Scalar(), true, false);

    cv::Mat detections;

    if (config_.use_cuda && ort_session_) {
        // Run on GPU via ONNX Runtime
        std::array<int64_t, 4> input_shape = {1, 3, config_.input_height, config_.input_width};
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            blob.ptr<float>(),
            1 * 3 * config_.input_height * config_.input_width,
            input_shape.data(),
            input_shape.size()
        );

        const char* input_names[] = {"images"};
        const char* output_names[] = {"output0"};

        auto output_tensors = ort_session_->Run(
            Ort::RunOptions{nullptr},
            input_names, &input_tensor, 1,
            output_names, 1
        );

        float* raw_output = output_tensors[0].GetTensorMutableData<float>();
        // YOLOv8 output is [1, 84, 8400]
        cv::Mat raw_mat(84, 8400, CV_32F, raw_output);
        detections = raw_mat.t(); // Transpose to [8400, 84]
    } else {
        // CPU fallback
        net_.setInput(blob);
        std::vector<cv::Mat> outputs;
        net_.forward(outputs, net_.getUnconnectedOutLayersNames());
        cv::Mat output_data = outputs[0];
        cv::Mat raw_mat(output_data.size[1], output_data.size[2], CV_32F, output_data.ptr<float>());
        detections = raw_mat.t();
    }

    const int rows = detections.rows;       // 8400
    const int dimensions = detections.cols; // 84

    const float x_factor = static_cast<float>(frame.cols) / static_cast<float>(config_.input_width);
    const float y_factor = static_cast<float>(frame.rows) / static_cast<float>(config_.input_height);

    std::vector<int> class_ids;
    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;

    for (int i = 0; i < rows; ++i) {
        float* row_ptr = detections.ptr<float>(i);
        float* scores = row_ptr + 4;

        cv::Mat scores_mat(1, dimensions - 4, CV_32F, scores);
        cv::Point class_id_point;
        double max_class_score = 0.0;
        cv::minMaxLoc(scores_mat, nullptr, &max_class_score, nullptr, &class_id_point);

        if (max_class_score > config_.confidence_threshold) {
            float cx = row_ptr[0];
            float cy = row_ptr[1];
            float w  = row_ptr[2];
            float h  = row_ptr[3];

            int left   = static_cast<int>((cx - 0.5f * w) * x_factor);
            int top    = static_cast<int>((cy - 0.5f * h) * y_factor);
            int width  = static_cast<int>(w * x_factor);
            int height = static_cast<int>(h * y_factor);

            left   = std::max(0, left);
            top    = std::max(0, top);
            width  = std::min(width, frame.cols - left);
            height = std::min(height, frame.rows - top);

            if (width > 0 && height > 0) {
                confidences.push_back(static_cast<float>(max_class_score));
                class_ids.push_back(class_id_point.x);
                boxes.emplace_back(left, top, width, height);
            }
        }
    }

    std::vector<int> nms_indices;
    cv::dnn::NMSBoxes(boxes, confidences, config_.score_threshold, config_.nms_threshold, nms_indices);

    std::vector<Detection> results;
    results.reserve(nms_indices.size());

    for (int idx : nms_indices) {
        Detection det;
        det.class_id = class_ids[idx];
        det.class_name = (det.class_id >= 0 && det.class_id < static_cast<int>(labels_.size()))
                             ? labels_[det.class_id] : "unknown";
        det.confidence = confidences[idx];
        det.box = boxes[idx];
        results.push_back(std::move(det));
    }

    return results;
}
