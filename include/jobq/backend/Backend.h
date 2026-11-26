#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "storage/JobRecord.h"

namespace jobq::backend {

using Duration = std::chrono::milliseconds;

struct JobId { std::uint64_t value; };
struct WorkerId { std::string value; };
struct OwnershipToken { std::string value; };
struct Generation { std::uint64_t value; };

enum class Error { NotFound, OwnershipLost, LeaseExpired, Conflict, Unavailable, Timeout, InvalidInput };

template<class T>
using Result = std::variant<T, Error>;
using Outcome = Result<std::monostate>;

struct Submission {
    std::string type;
    std::string payload;
    JobPriority priority{JobPriority::Normal};
    RetryPolicy retryPolicy{};
};

struct WorkerLease {
    WorkerId worker;
    OwnershipToken token;
};

struct JobLease {
    JobId job;
    WorkerLease owner;
    OwnershipToken token;
    Generation generation;
};

struct Claim {
    JobRecord record;
    JobLease lease;
};

struct FinalFailure {};
struct RetryAfter { Duration delay; };
using Failure = std::variant<FinalFailure, RetryAfter>;

enum class WorkerState { Registered, Expired };
struct WorkerRegistration { WorkerId worker; WorkerState state; };

struct Snapshot {
    std::uint64_t readyJobs;
    std::uint64_t delayedJobs;
    std::vector<WorkerRegistration> workers;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual Result<JobId> submit(Submission job) = 0;
    virtual Result<JobRecord> get(JobId id) const = 0;
    virtual Result<std::vector<JobRecord>> list() const = 0;

    virtual Result<WorkerLease> registerWorker(WorkerId worker, Duration ttl) = 0;
    virtual Outcome heartbeat(const WorkerLease& worker, Duration ttl) = 0;
    virtual Outcome unregisterWorker(const WorkerLease& worker) = 0;

    virtual Result<std::optional<Claim>> claim(const WorkerLease& worker, Duration ttl) = 0;
    virtual Outcome renew(const JobLease& lease, Duration ttl) = 0;
    virtual Outcome complete(const JobLease& lease) = 0;
    virtual Outcome fail(const JobLease& lease, Failure disposition) = 0;
    virtual Result<std::uint64_t> recoverExpiredClaims() = 0;

    virtual Result<Snapshot> snapshot() const = 0;
};

}
