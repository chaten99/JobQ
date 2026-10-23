#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace jobq {

struct JobMetricsSnapshot {
    std::uint64_t submittedJobs{0};
    std::uint64_t startedExecutions{0};
    std::uint64_t completedExecutions{0};
    std::uint64_t failedExecutions{0};
    std::uint64_t scheduledRetries{0};
    std::uint64_t finishedExecutions{0};

    std::chrono::nanoseconds totalExecutionTime{0};
    std::chrono::nanoseconds averageExecutionTime{0};
    std::chrono::nanoseconds elapsedTime{0};

    double throughputJobsPerSecond{0.0};
};

class JobMetrics {
public:
    using Clock = std::chrono::steady_clock;

    JobMetrics();

    JobMetrics(const JobMetrics&) = delete;
    JobMetrics& operator=(const JobMetrics&) = delete;

    void recordJobSubmitted();

    void recordExecutionStarted();

    void recordExecutionCompleted(Clock::duration executionTime);

    void recordExecutionFailed(Clock::duration executionTime);

    void recordRetryScheduled();

    [[nodiscard]] JobMetricsSnapshot snapshot() const;

private:
    void validateExecutionTime(Clock::duration executionTime) const;

    mutable std::mutex mutex_;

    std::uint64_t submittedJobs_{0};
    std::uint64_t startedExecutions_{0};
    std::uint64_t completedExecutions_{0};
    std::uint64_t failedExecutions_{0};
    std::uint64_t scheduledRetries_{0};

    std::chrono::nanoseconds totalExecutionTime_{0};

    const Clock::time_point startedAt_;
};

} // namespace jobq