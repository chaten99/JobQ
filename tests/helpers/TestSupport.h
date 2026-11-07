#pragma once

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "metrics/JobMetrics.h"

namespace jobq_test {

using namespace std::chrono_literals;

constexpr auto kTimeout = 10s;

inline int& testsRun() {
    static int value = 0;
    return value;
}

inline int& testsFailed() {
    static int value = 0;
    return value;
}

inline void check(bool condition, const std::string& description) {
    ++testsRun();

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed();
        std::cout << "[FAIL] " << description << '\n';
    }
}

template <typename Exception, typename Fn>
bool throwsWith(Fn&& fn, const std::string& fragment) {
    try {
        fn();
    } catch (const Exception& error) {
        return std::string(error.what()).find(fragment) !=
               std::string::npos;
    } catch (...) {
        return false;
    }
    return false;
}

inline bool waitUntil(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }

    return predicate();
}

class Gate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

    bool wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, kTimeout, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_{false};
};

class Counter {
public:
    void add() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++value_;
        }
        cv_.notify_all();
    }

    bool waitAtLeast(std::size_t target) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock, kTimeout, [&] { return value_ >= target; }
        );
    }

    std::size_t value() {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t value_{0};
};

inline bool metricsConsistent(const jobq::JobMetricsSnapshot& snap) {
    return snap.finishedExecutions ==
               snap.completedExecutions + snap.failedExecutions &&
           snap.startedExecutions == snap.finishedExecutions &&
           snap.scheduledRetries <= snap.failedExecutions &&
           snap.totalExecutionTime.count() >= 0 &&
           snap.averageExecutionTime.count() >= 0 &&
           std::isfinite(snap.throughputJobsPerSecond) &&
           snap.throughputJobsPerSecond >= 0.0;
}

}
