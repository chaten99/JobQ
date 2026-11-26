#pragma once

#include "jobq/backend/Backend.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace jobq::backend::test {

class FakeBackend final : public Backend {
    struct WorkerEntry { OwnershipToken token; Duration expiry; bool registered; };
    struct JobEntry {
        JobRecord record;
        Duration due;
        std::uint64_t order;
        Generation generation{0};
        std::optional<JobLease> lease;
        Duration expiry{};
        bool delayed{false};
    };

public:
    void advance(Duration delta) {
        std::lock_guard lock(mutex_);
        now_ += delta;
    }

    void inject(std::optional<Error> error) {
        std::lock_guard lock(mutex_);
        error_ = error;
    }

    Result<JobId> submit(Submission input) override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (input.type.empty()) return Error::InvalidInput;
        const auto id = nextId_;
        JobRecord record{id, std::move(input.type), std::move(input.payload), input.priority,
                         JobStatus::Pending, input.retryPolicy, 0};
        try { validateJobRecord(record); }
        catch (const std::invalid_argument&) { return Error::InvalidInput; }
        jobs_.emplace(id, JobEntry{std::move(record), now_, nextOrder_++, {0}, {}, {}, false});
        ++nextId_;
        return JobId{id};
    }

    Result<JobRecord> get(JobId id) const override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        const auto it = jobs_.find(id.value);
        if (it == jobs_.end()) return Error::NotFound;
        return it->second.record;
    }

    Result<std::vector<JobRecord>> list() const override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        std::vector<JobRecord> records;
        for (const auto& entry : jobs_) records.push_back(entry.second.record);
        return records;
    }

    Result<WorkerLease> registerWorker(WorkerId worker, Duration ttl) override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (ttl <= Duration::zero() || worker.value.empty()) return Error::InvalidInput;
        const auto it = workers_.find(worker.value);
        if (it != workers_.end() && it->second.registered && it->second.expiry > now_) return Error::Conflict;
        WorkerLease lease{std::move(worker), token()};
        workers_.insert_or_assign(lease.worker.value, WorkerEntry{lease.token, now_ + ttl, true});
        return lease;
    }

    Outcome heartbeat(const WorkerLease& worker, Duration ttl) override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (ttl <= Duration::zero()) return Error::InvalidInput;
        if (const auto error = workerError(worker)) return *error;
        workers_.at(worker.worker.value).expiry = now_ + ttl;
        return std::monostate{};
    }

    Outcome unregisterWorker(const WorkerLease& worker) override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (const auto error = workerError(worker)) return *error;
        workers_.at(worker.worker.value).registered = false;
        return std::monostate{};
    }

    Result<std::optional<Claim>> claim(const WorkerLease& worker, Duration ttl) override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (ttl <= Duration::zero()) return Error::InvalidInput;
        if (const auto error = workerError(worker)) return *error;
        promote();
        JobEntry* selected = nullptr;
        for (auto& [id, job] : jobs_) {
            if (job.record.status != JobStatus::Pending || job.delayed) continue;
            if (!selected || job.record.priority > selected->record.priority ||
                (job.record.priority == selected->record.priority && job.order < selected->order)) selected = &job;
        }
        if (!selected) return std::optional<Claim>{};
        auto record = selected->record;
        record.status = JobStatus::Running;
        ++record.attemptsMade;
        JobLease lease{{record.id}, worker, token(), {selected->generation.value + 1}};
        Claim result{record, lease};
        selected->lease.emplace(std::move(lease));
        selected->generation = selected->lease->generation;
        selected->expiry = now_ + ttl;
        selected->record.status = record.status;
        selected->record.attemptsMade = record.attemptsMade;
        return std::optional<Claim>{std::move(result)};
    }

    Outcome renew(const JobLease& lease, Duration ttl) override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (ttl <= Duration::zero()) return Error::InvalidInput;
        if (const auto error = claimError(lease)) return *error;
        jobs_.at(lease.job.value).expiry = now_ + ttl;
        return std::monostate{};
    }

    Outcome complete(const JobLease& lease) override {
        return finish(lease, JobStatus::Completed, {});
    }

    Outcome fail(const JobLease& lease, Failure disposition) override {
        if (const auto* retry = std::get_if<RetryAfter>(&disposition))
            return finish(lease, JobStatus::Pending, retry->delay);
        return finish(lease, JobStatus::Failed, {});
    }

    Result<std::uint64_t> recoverExpiredClaims() override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        promote();
        std::uint64_t recovered = 0;
        for (auto& [id, job] : jobs_) {
            if (!job.lease || (job.expiry > now_ && !workerError(job.lease->owner))) continue;
            job.lease.reset();
            job.record.status = JobStatus::Pending;
            job.order = nextOrder_++;
            ++recovered;
        }
        return recovered;
    }

    Result<Snapshot> snapshot() const override {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        Snapshot result{};
        for (const auto& [id, job] : jobs_) {
            if (job.record.status != JobStatus::Pending) continue;
            if (job.delayed && job.due > now_) ++result.delayedJobs;
            else ++result.readyJobs;
        }
        for (const auto& [id, worker] : workers_) {
            if (worker.registered)
                result.workers.push_back({{id}, worker.expiry <= now_ ? WorkerState::Expired : WorkerState::Registered});
        }
        return result;
    }

private:
    OwnershipToken token() { return {std::to_string(nextToken_++)}; }

    std::optional<Error> workerError(const WorkerLease& worker) const {
        const auto it = workers_.find(worker.worker.value);
        if (it == workers_.end() || !it->second.registered) return Error::NotFound;
        if (it->second.token.value != worker.token.value) return Error::OwnershipLost;
        if (it->second.expiry <= now_) return Error::LeaseExpired;
        return {};
    }

    std::optional<Error> claimError(const JobLease& lease) const {
        const auto it = jobs_.find(lease.job.value);
        if (it == jobs_.end()) return Error::NotFound;
        const auto& job = it->second;
        if (!job.lease || job.lease->token.value != lease.token.value ||
            job.lease->generation.value != lease.generation.value ||
            job.lease->owner.worker.value != lease.owner.worker.value ||
            job.lease->owner.token.value != lease.owner.token.value) return Error::OwnershipLost;
        if (job.expiry <= now_) return Error::LeaseExpired;
        if (auto error = workerError(lease.owner)) return *error == Error::NotFound ? Error::OwnershipLost : *error;
        return {};
    }

    void promote() {
        std::vector<JobEntry*> eligible;
        for (auto& [id, job] : jobs_) {
            if (job.delayed && job.due <= now_) eligible.push_back(&job);
        }
        std::sort(eligible.begin(), eligible.end(), [](const JobEntry* a, const JobEntry* b) {
            return std::tie(a->due, a->order) < std::tie(b->due, b->order);
        });
        for (auto* job : eligible) {
            job->delayed = false;
            job->order = nextOrder_++;
        }
    }

    Outcome finish(const JobLease& lease, JobStatus state, std::optional<Duration> delay) {
        std::lock_guard lock(mutex_);
        if (error_) return *error_;
        if (delay && *delay < Duration::zero()) return Error::InvalidInput;
        if (const auto error = claimError(lease)) return *error;
        auto& job = jobs_.at(lease.job.value);
        if (delay && job.record.attemptsMade > job.record.retryPolicy.maxRetries) return Error::Conflict;
        if (delay) {
            job.delayed = *delay > Duration::zero();
            job.due = now_ + *delay;
            job.order = nextOrder_++;
        }
        job.record.status = state;
        job.lease.reset();
        return std::monostate{};
    }

    mutable std::mutex mutex_;
    Duration now_{};
    std::optional<Error> error_;
    std::map<std::uint64_t, JobEntry> jobs_;
    std::map<std::string, WorkerEntry> workers_;
    std::uint64_t nextId_{1};
    std::uint64_t nextToken_{1};
    std::uint64_t nextOrder_{1};
};

}
