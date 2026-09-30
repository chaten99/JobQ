#pragma once

#include <cstdint>
#include <string>

namespace jobq {

enum class JobStatus {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

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

    void setStatus(JobStatus status);

private:
    std::uint64_t id_;
    std::string type_;
    std::string payload_;
    JobStatus status_;
};

} // namespace jobq