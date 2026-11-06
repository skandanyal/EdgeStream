#ifndef EDGESTREAM_RINGBUFFER_H
#define EDGESTREAM_RINGBUFFER_H

#include <atomic>
#include <condition_variable>
#include <mutex>

template <typename T, int Capacity, int Num_Producers> class RingBuffer {
private:
  T buffer[Capacity];
  int head_ = 0;  // points to oldest val
  int tail_ = 0;  // points to newest val
  int count_ = 0; // number of eles.

  std::mutex mt;
  std::condition_variable cv;
  int active_producers;
  std::atomic<bool> stopped_ = false;

public:
  RingBuffer() {
    static_assert(Capacity > 0, "Buffer size should be greater than 0.\n");
    static_assert(Num_Producers > 0,
                  "Number of threads should be greater than 0.\n");

    active_producers = Num_Producers;
  }

  bool pop(T &val) {
    std::unique_lock<std::mutex> lock(mt);

    cv.wait(lock, [this] { return count_ > 0 || active_producers == 0 || stopped_; });

    if (count_ == 0)
      return false;

    val = std::move(buffer[head_]);
    head_ = (head_ + 1) % Capacity;
    count_--;

    lock.unlock();
    cv.notify_all();

    return true;
  }

  bool push(T &val) {
    std::unique_lock<std::mutex> lock(mt);

    cv.wait(lock, [this] { return count_ < Capacity || stopped_;});

    if (stopped_) return false;

    buffer[tail_] = std::move(val);
    tail_ = (tail_ + 1) % Capacity;
    count_++;

    lock.unlock();
    cv.notify_all();

    return true;
  }

  void stop() {
    std::unique_lock<std::mutex> lock(mt);
    stopped_ = true;

    lock.unlock();
    cv.notify_all();
  }

  bool is_stopped() {
    return stopped_.load();
  }

  int get_size() {
    std::lock_guard<std::mutex> lock(mt);
    return count_;
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
