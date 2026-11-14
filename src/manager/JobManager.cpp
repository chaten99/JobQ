
#include "manager/JobManager.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "metrics/JobMetrics.h"
#include "metrics/MeasuredLock.h"
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
void recordJobSubmittedSafely(JobMetrics* metrics) noexcept {
    if (!metrics) {
        return;
    }

    try {
        metrics->recordJobSubmitted();
    } catch (...) {
    }
}

void recordRetryScheduledSafely(JobMetrics* metrics) noexcept {
    if (!metrics) {
        return;
    }

    try {
        metrics->recordRetryScheduled();
    } catch (...) {
    }
}

} // namespace

JobManager::JobManager(JobQueue& queue)
    : queue_(queue) {}

JobManager::JobManager(
    JobQueue& queue,
    JobMetrics& metrics
)
    : queue_(queue),
      metrics_(&metrics) {}

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

JobManager::JobManager(
    JobQueue& queue,
    std::shared_ptr<JobStorage> storage,
    JobMetrics& metrics
)
    : queue_(queue),
      storage_(std::move(storage)),
      metrics_(&metrics) {

    if (!storage_) {
        throw std::invalid_argument(
            "Persistent JobManager requires a storage instance"
        );
    }

    restoreStoredJobs();
}

void JobManager::restoreStoredJobs() {
    const auto records = storage_->loadAll();

    std::unordered_set<uint64_t> seenIds;
    seenIds.reserve(records.size());

    uint64_t nextId = 1;

    for (const auto& record : records) {
        try {
            validateJobRecord(record);
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error(
                "Stored job " + std::to_string(record.id) +
                " is invalid: " + error.what()
            );
        }

        if (record.id == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error(
                "Stored job ID prevents generating another unique ID"
            );
        }

        if (!seenIds.insert(record.id).second) {
            throw std::runtime_error(
                "Storage returned duplicate job IDs: " +
                std::to_string(record.id)
            );
        }

        if (record.id >= nextId) {
            nextId = record.id + 1;
        }
    }

    std::vector<std::shared_ptr<Job>> jobsToSchedule;
    jobsToSchedule.reserve(records.size());

    for (const auto& record : records) {
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

        jobs_.emplace(record.id, job);

        if (job->getStatus() == JobStatus::Running) {
            job->markFailed();
        }

        if (job->getStatus() == JobStatus::Failed &&
            job->canRetry()) {

            const bool prepared = job->prepareForRetry();

            if (!prepared) {
                throw std::runtime_error(
                    "Retryable recovered job could not be prepared"
                );
            }
        }

        if (job->getStatus() == JobStatus::Pending) {
            jobsToSchedule.push_back(job);
        }
    }

    nextId_.store(nextId, std::memory_order_relaxed);

    for (const auto& job : jobsToSchedule) {
        bool accepted = false;
        const bool isRetry = job->getAttemptsMade() > 0;

        if (!isRetry) {
            accepted = queue_.push(job);
        } else {
            accepted = queue_.pushRetry(
                job,
                job->getRetryDelay()
            );
        }

        if (!accepted) {
            throw std::runtime_error(
                "Failed to enqueue recovered job " +
                std::to_string(job->getId())
            );
        }

        if (isRetry) {
            recordRetryScheduledSafely(metrics_);
        }
    }
}

void JobManager::setContentionMetrics(ContentionMetrics* metrics) noexcept {
    contention_ = metrics;
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
        MeasuredExclusiveLock lock(registryMutex_, contention_, LockCategory::JobRegistry);
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
        MeasuredExclusiveLock lock(registryMutex_, contention_, LockCategory::JobRegistry);
        jobs_.erase(job->getId());
        throw;
    }

    if (queue_.push(job)) {
        recordJobSubmittedSafely(metrics_);
        return job;
    }

    try {
        job->markCancelled();
    } catch (...) {
        MeasuredExclusiveLock lock(registryMutex_, contention_, LockCategory::JobRegistry);
        jobs_.erase(job->getId());
        throw;
    }

    {
        MeasuredExclusiveLock lock(registryMutex_, contention_, LockCategory::JobRegistry);
        jobs_.erase(job->getId());
    }

    throw std::runtime_error(
        "Job submission failed because the queue is closed"
    );
}

std::shared_ptr<Job> JobManager::getJob(uint64_t id) const {
    MeasuredSharedLock lock(registryMutex_, contention_, LockCategory::JobRegistry);

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
    MeasuredSharedLock lock(registryMutex_, contention_, LockCategory::JobRegistry);
    return jobs_.size();
}

} // namespace jobq
