#include "job/Job.h"

#include <utility>

namespace jobq {

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
    return status_;
}

void Job::setStatus(JobStatus status) {
    status_ = status;
}

} // namespace jobq