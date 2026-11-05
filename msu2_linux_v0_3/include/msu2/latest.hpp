#pragma once
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
namespace msu2 {
// One waiting frame, not an unbounded FIFO. A completed in-flight frame is
// never interrupted; overwriting only affects a frame not yet handed off.
template<class T> class LatestFrame {
public:
    void put(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (value_) ++dropped_;
        value_=std::move(value); ready_.notify_one();
    }
    bool take(T& output) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock,[&]{ return closed_ || value_.has_value(); });
        if (error_) std::rethrow_exception(error_);
        if (!value_) return false;
        output=std::move(*value_); value_.reset(); return true;
    }
    void close(std::exception_ptr error={}) {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_=true; error_=error; ready_.notify_all();
    }
    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_); return dropped_;
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<T> value_;
    bool closed_=false;
    std::size_t dropped_=0;
    std::exception_ptr error_;
};
}
