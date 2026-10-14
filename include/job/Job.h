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

std::string_view toString(JobStatus status);

class Job {
public:
    Job(
        std::uint64_t id,
        std::string type,
        std::string payload
    );

    std::uint64_t getId() const;

    const std::string& getType() const;

    const std::string& getPayload() const;

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

    std::uint64_t id_;
    std::string type_;
    std::string payload_;

    mutable std::mutex statusMutex_;
    JobStatus status_;
};

} // namespace jobq