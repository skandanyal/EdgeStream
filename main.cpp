#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

#include <opencv2/opencv.hpp>

#include "include/ringBuffer.hpp"
#include "include/database.hpp"

constexpr int CAPACITY = 400;
constexpr int NUM_PRODS = 1;

void producer (RingBuffer<cv::Mat, CAPACITY, NUM_PRODS> &rb, std::string &source, int id) {
    cv::VideoCapture cap;
    if (source == "0") cap.open(0);
    else cap.open(source);

    if (!cap.isOpened()) {
        std::cerr << "Producer unable to open source.\n";
        rb.thread_finished();
        return;
    }

    cv::Mat frame;
    int frame_count = 0;

    while (!rb.is_stopped() && cap.read(frame)) {
        if (frame.empty()) break;
        if (!rb.push(frame)) break;

        frame_count++;
    }

    // std::osyncstream(std::cout) << "Producer " << id << " finished. Total frames: " << frame_count << "\n";
    cap.release();
    rb.thread_finished();
}

void consumer (RingBuffer<cv::Mat, CAPACITY, NUM_PRODS> &rb, Database &db, int id){
    cv::Mat frame;
    int displayed_frames = 0;

    auto last_time = std::chrono::high_resolution_clock::now();

    while (rb.pop(frame)) {
        if (frame.empty()) continue;

        // display frame
        cv::imshow("Live feed", frame);
        displayed_frames++;

        //calculate fps
        auto curr_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration<float>(curr_time - last_time).count();
        last_time = curr_time;
        float fps = (elapsed_time > 0.0f) ? static_cast<float>(1.0 / elapsed_time) : 30.0f;

        // calculate timestamp - time since epoch
        auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        // calculate queue size
        int queue_size = rb.get_size();

        db.insert_telemetry(displayed_frames, timestamp_ms, queue_size, fps);

        bool window_closed = cv::getWindowProperty("Live feed", cv::WND_PROP_VISIBLE) < 1;

        char key = (char) cv::waitKey(20);
        if (key == 27 || key == 'q' || key == 'Q' || window_closed) {
            rb.stop();
            break;
        }
    }

    rb.stop();
}

int main(int argc, char* argv[]) {
    std::string source = (argc > 1) ? argv[1] : "assets/sample_video.mp4";

    RingBuffer<cv::Mat, CAPACITY, NUM_PRODS> rb;
    Database db("database/EdgeStream_data.db");

    // Executing threads
    std::thread prod (producer, std::ref(rb), std::ref(source), 0);
    consumer(std::ref(rb), std::ref(db), 0);

    auto start_time = std::chrono::high_resolution_clock::now();
    prod.join();
    auto end_time = std::chrono::high_resolution_clock::now();

    auto time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    std::cout << "\n Database metrics statistics \n";
    db.display_telemetry_metrics();

    std::cout << "Pipeline finished successfully.\n";
    std::cout << "Time to exit pipeline (in us): " << time << "\n";

    return 0;
}
