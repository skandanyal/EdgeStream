#ifndef EDGESTREAM_RINGBUFFER_H
#define EDGESTREAM_RINGBUFFER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <cstdint>
#include <stdexcept>

template <typename T>
class RingBuffer {
private:
    std::vector<T> buffer_;
    const size_t capacity_;
    size_t head_ = 0;  // points to oldest val
    size_t tail_ = 0;  // points to newest val
    size_t count_ = 0; // number of elements

    std::mutex mt;
    std::condition_variable cv;
    int active_producers;
    std::atomic<bool> stopped_{false};
    std::atomic<uint64_t> dropped_frames_{0};

public:
    explicit RingBuffer(size_t capacity = 50, int num_producers = 1)
        : buffer_(capacity), capacity_(capacity), active_producers(num_producers) {
        if (capacity_ == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0.\n");
        }
        if (active_producers <= 0) {
            throw std::invalid_argument("RingBuffer num_producers must be greater than 0.\n");
        }
    }

    bool pop(T &val) {
        std::unique_lock<std::mutex> lock(mt);

        cv.wait(lock, [this] { return count_ > 0 || active_producers == 0 || stopped_.load(); });

        if (count_ == 0)
          return false;

        val = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        count_--;

        lock.unlock();
        cv.notify_all();

        return true;
    }

    bool push(T &val, bool drop_oldest = true) {
        std::unique_lock<std::mutex> lock(mt);

        if (drop_oldest && count_ == capacity_) {
            // Overwrite / drop the oldest frame to prevent latency spikes
            head_ = (head_ + 1) % capacity_;
            count_--;
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
        } else {
            cv.wait(lock, [this] { return count_ < capacity_ || stopped_.load(); });
        }

        if (stopped_.load()) return false;

        buffer_[tail_] = std::move(val);
        tail_ = (tail_ + 1) % capacity_;
        count_++;

        lock.unlock();
        cv.notify_all();

        return true;
    }

    [[nodiscard]] uint64_t get_dropped_count() const {
        return dropped_frames_.load();
    }

    void stop() {
        std::unique_lock<std::mutex> lock(mt);
        stopped_ = true;

        lock.unlock();
        cv.notify_all();
    }

    bool is_stopped() const {
        return stopped_.load();
    }

    size_t get_size() {
        std::lock_guard<std::mutex> lock(mt);
        return count_;
    }

    size_t get_capacity() const {
        return capacity_;
    }

    void thread_finished() {
        {
            std::lock_guard<std::mutex> lock(mt);
            --active_producers;
        }
        cv.notify_all();
    }
};

#endif // EDGESTREAM_RINGBUFFER_H
