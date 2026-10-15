#include "job/Job.h"

#include <stdexcept>
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

std::string_view toString(JobPriority priority) {
    switch (priority) {
        case JobPriority::Low:
            return "LOW";

        case JobPriority::Normal:
            return "NORMAL";

        case JobPriority::High:
            return "HIGH";
    }

    return "UNKNOWN";
}

Job::Job(
    uint64_t id,
    std::string type,
    std::string payload,
    JobPriority priority
)
    : id_(id),
      type_(std::move(type)),
      payload_(std::move(payload)),
      priority_(priority),
      status_(JobStatus::Pending) {}

uint64_t Job::getId() const {
    return id_;
}

const std::string& Job::getType() const {
    return type_;
}

const std::string& Job::getPayload() const {
    return payload_;
}

JobPriority Job::getPriority() const {
    return priority_;
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

}
