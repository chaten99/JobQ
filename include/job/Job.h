#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace jobq {

enum class JobStatus {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

enum class JobPriority {
    Low = 0,
    Normal = 1,
    High = 2
};

struct RetryPolicy {
    std::size_t maxRetries{0};
    std::chrono::milliseconds initialDelay{100};

    double multiplier{2.0};

    std::chrono::milliseconds maxDelay{5000};
};

std::string_view toString(JobStatus status);
std::string_view toString(JobPriority priority);

class Job {
public:
    Job(
        uint64_t id,
        std::string type,
        std::string payload,
        JobPriority priority = JobPriority::Normal,
        RetryPolicy retryPolicy = {}
    );

    uint64_t getId() const;

    const std::string& getType() const;

    const std::string& getPayload() const;

    JobPriority getPriority() const;

    RetryPolicy getRetryPolicy() const;

    std::size_t getAttemptsMade() const;

    JobStatus getStatus() const;

    bool canRetry() const;

    std::chrono::milliseconds getRetryDelay() const;

    bool prepareForRetry();

    void markRunning();

    void markCompleted();

    void markFailed();

    void markCancelled();

private:
    void transitionTo(
        JobStatus expectedCurrent,
        JobStatus newStatus
    );

    uint64_t id_;
    std::string type_;
    std::string payload_;

    JobPriority priority_;
    RetryPolicy retryPolicy_;

    std::size_t attemptsMade_{0};

    mutable std::mutex statusMutex_;
    JobStatus status_;
};

}