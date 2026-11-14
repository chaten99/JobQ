#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "metrics/ContentionMetrics.h"

namespace jobq {

template <typename Mutex, typename Lock>
class MeasuredLock {
public:
    using Clock = std::chrono::steady_clock;

    MeasuredLock(
        Mutex& mutex,
        ContentionMetrics* metrics,
        LockCategory category
    )
        : lock_(mutex, std::defer_lock),
          metrics_(metrics),
          category_(category) {
        if (!metrics_) {
            lock_.lock();
            return;
        }

        if (lock_.try_lock()) {
            holdStart_ = Clock::now();
        } else {
            const auto attempt = Clock::now();
            lock_.lock();
            holdStart_ = Clock::now();
            pendingWait_ = toNanoseconds(holdStart_ - attempt);
            pendingBlocked_ = true;
        }

        pendingAcquisition_ = true;
        timing_ = true;
    }

    ~MeasuredLock() {
        if (!metrics_) {
            return;
        }

        const auto end = Clock::now();

        if (lock_.owns_lock()) {
            lock_.unlock();
        }

        flush(end);
    }

    MeasuredLock(const MeasuredLock&) = delete;
    MeasuredLock& operator=(const MeasuredLock&) = delete;

    Lock& native() noexcept { return lock_; }

    void pauseHold() noexcept {
        if (metrics_ && timing_) {
            flush(Clock::now());
        }
    }

    void resumeHold() noexcept {
        if (metrics_) {
            holdStart_ = Clock::now();
            timing_ = true;
        }
    }

private:
    static std::uint64_t toNanoseconds(Clock::duration duration) noexcept {
        const auto count =
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                .count();

        return count < 0 ? 0 : static_cast<std::uint64_t>(count);
    }

    void flush(Clock::time_point end) noexcept {
        if (pendingAcquisition_) {
            metrics_->recordAcquisition(
                category_, pendingWait_, pendingBlocked_
            );
            pendingAcquisition_ = false;
            pendingWait_ = 0;
            pendingBlocked_ = false;
        }

        if (timing_) {
            metrics_->recordHold(category_, toNanoseconds(end - holdStart_));
            timing_ = false;
        }
    }

    Lock lock_;
    ContentionMetrics* metrics_;
    LockCategory category_;
    Clock::time_point holdStart_{};
    std::uint64_t pendingWait_{0};
    bool pendingBlocked_{false};
    bool pendingAcquisition_{false};
    bool timing_{false};
};

using MeasuredMutexLock = MeasuredLock<std::mutex, std::unique_lock<std::mutex>>;

using MeasuredExclusiveLock =
    MeasuredLock<std::shared_mutex, std::unique_lock<std::shared_mutex>>;

using MeasuredSharedLock =
    MeasuredLock<std::shared_mutex, std::shared_lock<std::shared_mutex>>;

}
