
#pragma once

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

std::string_view toString(JobStatus status);
std::string_view toString(JobPriority priority);

class Job {
public:
    Job(
        uint64_t id,
        std::string type,
        std::string payload,
        JobPriority priority = JobPriority::Normal
    );

    uint64_t getId() const;

    const std::string& getType() const;

    const std::string& getPayload() const;

    JobPriority getPriority() const;

    JobStatus getStatus() const;

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

    mutable std::mutex statusMutex_;
    JobStatus status_;
};

}
