#include "job/Job.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace jobq {

std::string_view toString(JobStatus status) {
    switch (status) {
        case JobStatus::Pending:
            return "PENDING";

        case JobStatus::Running:
            return "RUNNING";

        case JobStatus::Completed:
            return "COMPLETED";

        case JobStatus::Failed:
            return "FAILED";

        case JobStatus::Cancelled:
            return "CANCELLED";
    }

    return "UNKNOWN";
}

Job::Job(
    std::uint64_t id,
    std::string type,
    std::string payload
)
    : id_(id),
      type_(std::move(type)),
      payload_(std::move(payload)),
      status_(JobStatus::Pending) {
}

std::uint64_t Job::getId() const {
    return id_;
}

const std::string& Job::getType() const {
    return type_;
}

const std::string& Job::getPayload() const {
    return payload_;
}

JobStatus Job::getStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void Job::transitionTo(
    JobStatus expectedCurrent,
    JobStatus newStatus
) {
    std::lock_guard<std::mutex> lock(statusMutex_);

    if (status_ != expectedCurrent) {
        throw std::logic_error(
            "Invalid job state transition: " +
            std::string(toString(status_)) +
            " -> " +
            std::string(toString(newStatus))
        );
    }

    status_ = newStatus;
}

void Job::markRunning() {
    transitionTo(
        JobStatus::Pending,
        JobStatus::Running
    );
}

void Job::markCompleted() {
    transitionTo(
        JobStatus::Running,
        JobStatus::Completed
    );
}

void Job::markFailed() {
    transitionTo(
        JobStatus::Running,
        JobStatus::Failed
    );
}

void Job::markCancelled() {
    transitionTo(
        JobStatus::Pending,
        JobStatus::Cancelled
    );
}

} // namespace jobq