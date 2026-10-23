#include "metrics/JobMetrics.h"

#include <stdexcept>

namespace jobq {

JobMetrics::JobMetrics()
    : startedAt_(Clock::now()) {
}

void JobMetrics::recordJobSubmitted() {
    std::lock_guard<std::mutex> lock(mutex_);

    ++submittedJobs_;
}

void JobMetrics::recordExecutionStarted() {
    std::lock_guard<std::mutex> lock(mutex_);

    ++startedExecutions_;
}

void JobMetrics::validateExecutionTime(
    Clock::duration executionTime
) const {
    if (executionTime < Clock::duration::zero()) {
        throw std::invalid_argument(
            "Execution time cannot be negative"
        );
    }
}

void JobMetrics::recordExecutionCompleted(
    Clock::duration executionTime
) {
    validateExecutionTime(executionTime);

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            executionTime
        );

    std::lock_guard<std::mutex> lock(mutex_);

    ++completedExecutions_;
    totalExecutionTime_ += elapsed;
}

void JobMetrics::recordExecutionFailed(
    Clock::duration executionTime
) {
    validateExecutionTime(executionTime);

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            executionTime
        );

    std::lock_guard<std::mutex> lock(mutex_);

    ++failedExecutions_;
    totalExecutionTime_ += elapsed;
}

void JobMetrics::recordRetryScheduled() {
    std::lock_guard<std::mutex> lock(mutex_);

    ++scheduledRetries_;
}

JobMetricsSnapshot JobMetrics::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    JobMetricsSnapshot result;

    result.submittedJobs = submittedJobs_;
    result.startedExecutions = startedExecutions_;
    result.completedExecutions = completedExecutions_;
    result.failedExecutions = failedExecutions_;
    result.scheduledRetries = scheduledRetries_;

    result.finishedExecutions =
        completedExecutions_ + failedExecutions_;

    result.totalExecutionTime = totalExecutionTime_;

    if (result.finishedExecutions > 0) {
        const auto averageNanoseconds =
            totalExecutionTime_.count() /
            static_cast<std::chrono::nanoseconds::rep>(
                result.finishedExecutions
            );

        result.averageExecutionTime =
            std::chrono::nanoseconds(averageNanoseconds);
    }

    result.elapsedTime =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - startedAt_
        );

    const double elapsedSeconds =
        std::chrono::duration<double>(
            result.elapsedTime
        ).count();

    if (elapsedSeconds > 0.0) {
        result.throughputJobsPerSecond =
            static_cast<double>(result.finishedExecutions) /
            elapsedSeconds;
    }

    return result;
}

} // namespace jobq