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

namespace jobq {

class JobManager {
public:
    JobManager() = default;

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::shared_ptr<Job> createJob(
        std::string type,
        std::string payload
    );

    std::shared_ptr<Job> getJob(
        std::uint64_t id
    ) const;

    std::optional<JobStatus> getStatus(
        std::uint64_t id
    ) const;

    std::size_t totalJobs() const;

private:
    std::atomic<std::uint64_t> nextId_{1};

    mutable std::shared_mutex registryMutex_;

    std::unordered_map<
        std::uint64_t,
        std::shared_ptr<Job>
    > jobs_;
};

} // namespace jobq