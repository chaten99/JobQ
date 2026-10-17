
#include "manager/JobManager.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "storage/JobRecord.h"

namespace jobq {

namespace {

JobRecord toJobRecord(const JobSnapshot& snapshot) {
    JobRecord record;

    record.id = snapshot.id;
    record.type = snapshot.type;
    record.payload = snapshot.payload;
    record.priority = snapshot.priority;
    record.status = snapshot.status;
    record.retryPolicy = snapshot.retryPolicy;
    record.attemptsMade = snapshot.attemptsMade;

    return record;
}

JobStateObserver makeObserver(
    const std::shared_ptr<JobStorage>& storage
) {
    if (!storage) {
        return {};
    }

    return [storage](const JobSnapshot& snapshot) {
        storage->save(toJobRecord(snapshot));
    };
}

} // namespace

JobManager::JobManager(JobQueue& queue)
    : queue_(queue) {}

JobManager::JobManager(
    JobQueue& queue,
    std::shared_ptr<JobStorage> storage
)
    : queue_(queue),
      storage_(std::move(storage)) {

    if (!storage_) {
        throw std::invalid_argument(
            "Persistent JobManager requires a storage instance"
        );
    }

    restoreStoredJobs();
}

void JobManager::restoreStoredJobs() {
    const auto records = storage_->loadAll();

    uint64_t nextId = 1;

    for (const auto& record : records) {
        if (record.id == 0) {
            throw std::runtime_error(
                "Storage returned a job with ID zero"
            );
        }

        if (record.id ==
            std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error(
                "Stored job ID prevents generating another unique ID"
            );
        }

        auto job = std::make_shared<Job>(
            record.id,
            record.type,
            record.payload,
            record.priority,
            record.retryPolicy,
            record.status,
            record.attemptsMade,
            createStateObserver()
        );

        const auto [iterator, inserted] =
            jobs_.emplace(record.id, std::move(job));

        if (!inserted) {
            throw std::runtime_error(
                "Storage returned duplicate job IDs"
            );
        }

        (void)iterator;

        if (record.id >= nextId) {
            nextId = record.id + 1;
        }
    }

    nextId_.store(nextId, std::memory_order_relaxed);
}

JobStateObserver JobManager::createStateObserver() const {
    return makeObserver(storage_);
}

void JobManager::persistSnapshot(
    const JobSnapshot& snapshot
) const {
    if (storage_) {
        storage_->save(toJobRecord(snapshot));
    }
}

std::shared_ptr<Job> JobManager::createJob(
    std::string type,
    std::string payload,
    JobPriority priority,
    RetryPolicy retryPolicy
) {
    const auto id = nextId_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    auto job = std::make_shared<Job>(
        id,
        std::move(type),
        std::move(payload),
        priority,
        retryPolicy,
        JobStatus::Pending,
        0,
        createStateObserver()
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
    JobPriority priority,
    RetryPolicy retryPolicy
) {
    auto job = createJob(
        std::move(type),
        std::move(payload),
        priority,
        retryPolicy
    );

    try {
        persistSnapshot(job->snapshot());
    } catch (...) {
        std::unique_lock<std::shared_mutex> lock(registryMutex_);
        jobs_.erase(job->getId());
        throw;
    }

    if (queue_.push(job)) {
        return job;
    }

    try {
        job->markCancelled();
    } catch (...) {
        std::unique_lock<std::shared_mutex> lock(registryMutex_);
        jobs_.erase(job->getId());

        throw;
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
