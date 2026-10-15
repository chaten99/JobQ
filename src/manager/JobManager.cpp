#include "manager/JobManager.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace jobq {

std::shared_ptr<Job> JobManager::createJob(
    std::string type,
    std::string payload,
    JobPriority priority
) {
    const auto id = nextId_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    auto job = std::make_shared<Job>(
        id,
        std::move(type),
        std::move(payload),
        priority
    );

    {
        std::unique_lock<std::shared_mutex> lock(registryMutex_);
        jobs_.emplace(id, job);
    }

    return job;
}

std::shared_ptr<Job> JobManager::submit(
    std::string type,
    std::string payload,
    JobPriority priority
) {
    auto job = createJob(
        std::move(type),
        std::move(payload),
        priority
    );

    if (queue_.push(job)) {
        return job;
    }

    {
        std::unique_lock<std::shared_mutex> lock(registryMutex_);
        jobs_.erase(job->getId());
    }

    throw std::runtime_error(
        "Job submission failed because the queue is closed"
    );
}

std::shared_ptr<Job> JobManager::getJob(uint64_t id) const {
    std::shared_lock<std::shared_mutex> lock(registryMutex_);

    const auto it = jobs_.find(id);

    if (it == jobs_.end()) {
        return nullptr;
    }

    return it->second;
}

std::optional<JobStatus> JobManager::getStatus(
    uint64_t id
) const {
    auto job = getJob(id);

    if (!job) {
        return std::nullopt;
    }

    return job->getStatus();
}

std::size_t JobManager::totalJobs() const {
    std::shared_lock<std::shared_mutex> lock(registryMutex_);

    return jobs_.size();
}

}