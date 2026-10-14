#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "job/Job.h"
#include "queue/JobQueue.h"

namespace jobq {

class JobManager {
public:
    explicit JobManager(JobQueue& queue)
        : queue_(queue) {}

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::shared_ptr<Job> createJob(
        std::string type,
        std::string payload
    );

    std::shared_ptr<Job> submit(
        std::string type,
        std::string payload
    );

    std::shared_ptr<Job> getJob(uint64_t id) const;

    std::optional<JobStatus> getStatus(uint64_t id) const;

    size_t totalJobs() const;

private:
    JobQueue& queue_;

    std::atomic<uint64_t> nextId_{1};

    mutable std::shared_mutex registryMutex_;

    std::unordered_map<uint64_t, std::shared_ptr<Job>> jobs_;
};

}