#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"
#include "storage/JobStorage.h"

namespace {

using namespace std::chrono_literals;
using jobq::JobPriority;
using jobq::JobRecord;
using jobq::JobStatus;
using jobq::RetryPolicy;

int testsRun = 0;
int testsFailed = 0;

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

template <typename Exception, typename Fn>
bool throwsWith(Fn&& fn, const std::string& fragment) {
    try {
        fn();
    } catch (const Exception& error) {
        return std::string(error.what()).find(fragment) !=
               std::string::npos;
    } catch (...) {
        return false;
    }
    return false;
}

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(
              std::filesystem::temp_directory_path() /
              ("jobq_recovery_hardening_" + name)
          ) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path journal() const {
        return path_ / "jobs.journal";
    }

private:
    std::filesystem::path path_;
};

class DoubleStorage final : public jobq::JobStorage {
public:
    void save(const JobRecord& record) override {
        if (failSave) {
            throw std::runtime_error("simulated write failure");
        }
        saved.push_back(record);
    }

    std::vector<JobRecord> loadAll() override {
        if (failLoad) {
            throw std::runtime_error("simulated read failure");
        }
        return records;
    }

    std::vector<JobRecord> records;
    std::vector<JobRecord> saved;
    bool failLoad{false};
    bool failSave{false};
};

RetryPolicy slowRetry(std::size_t maxRetries) {
    RetryPolicy policy;
    policy.maxRetries = maxRetries;
    policy.initialDelay = 5000ms;
    policy.multiplier = 2.0;
    policy.maxDelay = 60000ms;
    return policy;
}

RetryPolicy instantRetry(std::size_t maxRetries) {
    RetryPolicy policy;
    policy.maxRetries = maxRetries;
    policy.initialDelay = 0ms;
    policy.maxDelay = 0ms;
    return policy;
}

JobRecord makeRecord(
    std::uint64_t id,
    JobStatus status,
    std::size_t attempts,
    RetryPolicy policy = {},
    JobPriority priority = JobPriority::Normal
) {
    JobRecord record;
    record.id = id;
    record.type = "TYPE-" + std::to_string(id);
    record.payload = "payload-" + std::to_string(id);
    record.priority = priority;
    record.status = status;
    record.retryPolicy = policy;
    record.attemptsMade = attempts;
    return record;
}

std::vector<std::uint64_t> drainIds(jobq::JobQueue& queue) {
    std::vector<std::uint64_t> ids;
    while (auto job = queue.tryPop()) {
        ids.push_back(job->getId());
    }
    return ids;
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> fields;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, '|')) {
        fields.push_back(current);
    }
    return fields;
}

std::string mutateField(
    const std::string& serialized,
    std::size_t index,
    const std::string& value
) {
    auto fields = split(serialized);
    fields.at(index) = value;
    std::string result;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        result += (i ? "|" : "") + fields[i];
    }
    return result;
}

bool rejects(const std::string& serialized) {
    try {
        jobq::deserializeJobRecord(serialized);
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void testMultiplePendingRecovery() {
    std::cout << "\n--- Test A: Multiple Pending Jobs Recovery ---\n";

    TempDir dir("a");
    std::vector<JobRecord> original = {
        makeRecord(1, JobStatus::Pending, 0, slowRetry(3), JobPriority::Low),
        makeRecord(2, JobStatus::Pending, 0, instantRetry(1), JobPriority::High),
        makeRecord(3, JobStatus::Pending, 0, {}, JobPriority::Normal),
    };
    {
        jobq::FileJobStorage storage(dir.journal());
        for (const auto& record : original) {
            storage.save(record);
        }
    }

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(
        queue,
        std::make_shared<jobq::FileJobStorage>(dir.journal()),
        metrics
    );

    check(manager.totalJobs() == 3, "All pending jobs registered");
    check(queue.size() == 3, "All pending jobs queued");

    bool fieldsMatch = true;
    for (const auto& record : original) {
        const auto job = manager.getJob(record.id);
        fieldsMatch = fieldsMatch && job &&
            job->getType() == record.type &&
            job->getPayload() == record.payload &&
            job->getPriority() == record.priority &&
            job->getStatus() == JobStatus::Pending &&
            job->getAttemptsMade() == 0 &&
            job->getRetryPolicy().maxRetries ==
                record.retryPolicy.maxRetries &&
            job->getRetryPolicy().initialDelay ==
                record.retryPolicy.initialDelay;
    }
    check(fieldsMatch, "IDs and job fields preserved");
    check((drainIds(queue) == std::vector<std::uint64_t>{2, 3, 1}),
          "Queued in priority order");
    check(metrics.snapshot().submittedJobs == 0,
          "Recovery does not count restored jobs as submitted");
}

void testInterruptedRunningRecovery() {
    std::cout << "\n--- Test B: Interrupted Running Job Recovery ---\n";

    DoubleStorage storage;
    storage.records = {
        makeRecord(1, JobStatus::Running, 1, slowRetry(2)),
        makeRecord(2, JobStatus::Running, 1, {}),
        makeRecord(3, JobStatus::Running, 3, slowRetry(2)),
    };
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, shared, metrics);

    const auto retried = manager.getJob(1);
    const auto exhausted = manager.getJob(2);
    const auto lastAttempt = manager.getJob(3);

    check(retried->getStatus() == JobStatus::Pending &&
              retried->getAttemptsMade() == 1,
          "Retryable interrupted job becomes Pending, attempts kept");
    check(exhausted->getStatus() == JobStatus::Failed &&
              exhausted->getAttemptsMade() == 1,
          "Interrupted job without retries stays Failed");
    check(lastAttempt->getStatus() == JobStatus::Failed &&
              lastAttempt->getAttemptsMade() == 3,
          "Interrupted final attempt stays Failed");
    check(queue.size() == 1 && queue.tryPop() == nullptr,
          "Only the retryable job is queued, and it is delayed");
    check(metrics.snapshot().scheduledRetries == 1,
          "Exactly one retry counted as scheduled");

    std::vector<JobStatus> persistedForFirst;
    for (const auto& record : storage.saved) {
        if (record.id == 1) {
            persistedForFirst.push_back(record.status);
        }
    }
    check((persistedForFirst ==
           std::vector<JobStatus>{JobStatus::Failed, JobStatus::Pending}),
          "Failed then Pending snapshots persisted for the retry");
}

void testRetryableFailedRecovery() {
    std::cout << "\n--- Test C: Retryable Failed Job Recovery ---\n";

    DoubleStorage storage;
    storage.records = {
        makeRecord(1, JobStatus::Failed, 1, slowRetry(3)),
        makeRecord(2, JobStatus::Failed, 2, instantRetry(3)),
    };
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    jobq::JobManager manager(queue, shared);

    check(manager.getJob(1)->getStatus() == JobStatus::Pending &&
              manager.getJob(1)->getAttemptsMade() == 1,
          "Delayed retry prepared with attempts preserved");
    check(manager.getJob(2)->getAttemptsMade() == 2,
          "Second job attempts preserved");
    check(queue.size() == 2, "Both retries tracked by the queue");

    const auto first = queue.tryPop();
    check(first && first->getId() == 2, "Zero-delay retry available at once");
    check(queue.tryPop() == nullptr,
          "Delayed retry not executable before its delay expires");
    check(queue.size() == 1, "Delayed retry still pending, not duplicated");
}

void testExhaustedRetryRecovery() {
    std::cout << "\n--- Test D: Exhausted Retry Recovery ---\n";

    DoubleStorage storage;
    storage.records = {
        makeRecord(1, JobStatus::Failed, 1, {}),
        makeRecord(2, JobStatus::Failed, 3, slowRetry(2)),
    };
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, shared, metrics);

    check(manager.getStatus(1) == JobStatus::Failed &&
              manager.getStatus(2) == JobStatus::Failed,
          "Exhausted jobs remain Failed");
    check(queue.empty(), "Exhausted jobs are not requeued");
    check(storage.saved.empty(), "Recovery persists nothing for them");
    check(metrics.snapshot().scheduledRetries == 0,
          "No retry counted for exhausted jobs");
}

void testCompletedRecovery() {
    std::cout << "\n--- Test E: Completed Job Recovery ---\n";

    DoubleStorage storage;
    storage.records = {makeRecord(1, JobStatus::Completed, 2, slowRetry(3))};
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, shared, metrics);

    check(manager.getStatus(1) == JobStatus::Completed &&
              manager.getJob(1)->getAttemptsMade() == 2,
          "Completed job restored with attempts");
    check(queue.empty(), "Completed job not requeued");
    const auto snap = metrics.snapshot();
    check(snap.submittedJobs == 0 && snap.startedExecutions == 0 &&
              snap.completedExecutions == 0 && snap.scheduledRetries == 0,
          "Metrics untouched by restored completed record");
}

void testCancelledRecovery() {
    std::cout << "\n--- Test F: Cancelled Job Recovery ---\n";

    DoubleStorage storage;
    storage.records = {makeRecord(1, JobStatus::Cancelled, 0)};
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    jobq::JobManager manager(queue, shared);

    check(manager.getStatus(1) == JobStatus::Cancelled,
          "Cancelled job restored as Cancelled");
    check(queue.empty(), "Cancelled job not scheduled");
}

void testDuplicateIds() {
    std::cout << "\n--- Test G: Duplicate Job IDs ---\n";

    DoubleStorage storage;
    storage.records = {
        makeRecord(5, JobStatus::Pending, 0),
        makeRecord(5, JobStatus::Pending, 0),
    };
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    check(throwsWith<std::runtime_error>(
              [&] { jobq::JobManager manager(queue, shared); },
              "duplicate job IDs: 5"),
          "Duplicate IDs reported with a meaningful error");
    check(queue.empty(), "No duplicate job scheduled");
    check(storage.saved.empty(), "Nothing persisted while rejecting");

    TempDir dir("g");
    {
        jobq::FileJobStorage file(dir.journal());
        file.save(makeRecord(1, JobStatus::Pending, 0));
        file.save(makeRecord(1, JobStatus::Completed, 1));
    }
    jobq::JobQueue queue2;
    jobq::JobManager manager(
        queue2, std::make_shared<jobq::FileJobStorage>(dir.journal())
    );
    check(manager.totalJobs() == 1 &&
              manager.getStatus(1) == JobStatus::Completed,
          "Journal updates for one ID resolve to the latest record");
    check(queue2.empty(), "Superseded Pending record not requeued");
}

void testInvalidRecords() {
    std::cout << "\n--- Test H: Invalid Record Handling ---\n";

    const auto valid = jobq::serializeJobRecord(
        makeRecord(1, JobStatus::Pending, 0, slowRetry(2))
    );
    check(!rejects(valid), "Baseline serialized record is accepted");
    check(rejects(mutateField(valid, 0, "JOBQ9")), "Unknown version rejected");
    check(rejects(mutateField(valid, 1, "0")), "Zero ID rejected");
    check(rejects(mutateField(valid, 1, "abc")), "Non-numeric ID rejected");
    check(rejects(mutateField(valid, 1, "")), "Missing ID rejected");
    check(rejects(mutateField(valid, 2, "BOGUS")), "Invalid status rejected");
    check(rejects(mutateField(valid, 2, "pending")),
          "Status is case-sensitive");
    check(rejects(mutateField(valid, 3, "URGENT")),
          "Invalid priority rejected");
    check(rejects(mutateField(valid, 4, "-1")),
          "Negative attempts rejected");
    check(rejects(mutateField(valid, 4, "3")),
          "Pending attempts beyond retry policy rejected");
    check(rejects(mutateField(valid, 6, "-5")),
          "Negative initial delay rejected");
    check(rejects(mutateField(valid, 7, "0.5")),
          "Multiplier below 1 rejected");
    check(rejects(mutateField(valid, 7, "nan")),
          "Non-finite multiplier rejected");
    check(rejects(mutateField(valid, 8, "-1")),
          "Negative max delay rejected");
    check(rejects(mutateField(valid, 9, "ABC")), "Odd-length hex rejected");
    check(rejects(mutateField(valid, 10, "ZZ")), "Non-hex payload rejected");
    check(rejects("JOBQ1|1|PENDING"), "Missing fields rejected");
    check(rejects(valid.substr(0, valid.size() / 2)), "Truncated record rejected");
    check(rejects(""), "Empty record rejected");
    check(rejects(mutateField(
              jobq::serializeJobRecord(makeRecord(2, JobStatus::Running, 1)),
              4, "0")),
          "Running job without an attempt rejected");
    check(rejects(mutateField(
              jobq::serializeJobRecord(makeRecord(3, JobStatus::Failed, 1)),
              4, "5")),
          "Failed job beyond its allowed attempts rejected");

    JobRecord bad = makeRecord(1, JobStatus::Pending, 0);
    bad.status = static_cast<JobStatus>(99);
    bool serializeRejected = false;
    try {
        jobq::serializeJobRecord(bad);
    } catch (const std::invalid_argument&) {
        serializeRejected = true;
    }
    check(serializeRejected, "Out-of-range enum rejected at serialization");

    {
        DoubleStorage storage;
        storage.records = {
            makeRecord(1, JobStatus::Running, 1, slowRetry(2)),
            makeRecord(2, JobStatus::Pending, 9, slowRetry(1)),
        };
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        jobq::JobQueue queue;
        check(throwsWith<std::runtime_error>(
                  [&] { jobq::JobManager manager(queue, shared); },
                  "Stored job 2 is invalid"),
              "Manager rejects an invalid record with its ID in the error");
        check(storage.saved.empty() && queue.empty(),
              "Nothing persisted or queued before validation completes");
    }

    {
        DoubleStorage storage;
        storage.records = {makeRecord(1, JobStatus::Pending, 0)};
        storage.records[0].priority = static_cast<JobPriority>(42);
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        jobq::JobQueue queue;
        check(throwsWith<std::runtime_error>(
                  [&] { jobq::JobManager manager(queue, shared); },
                  "invalid"),
              "Manager rejects an out-of-range priority");
    }

    TempDir dir("h");
    {
        std::ofstream out(dir.journal(), std::ios::binary);
        out << valid << '\n'
            << "JOBQ1|2|BOGUS|NORMAL|0|0|100|2|5000|41|42\n";
    }
    jobq::JobQueue queue;
    check(throwsWith<std::runtime_error>(
              [&] {
                  jobq::JobManager manager(
                      queue,
                      std::make_shared<jobq::FileJobStorage>(dir.journal())
                  );
              },
              "line 2"),
          "Corrupt journal line fails recovery and names the line");
    check(queue.empty(), "No jobs scheduled from a corrupt journal");

    TempDir torn("h_torn");
    {
        std::ofstream out(torn.journal(), std::ios::binary);
        out << valid << '\n' << "JOBQ1|2|PEND";
    }
    jobq::JobQueue queue2;
    jobq::JobManager manager(
        queue2, std::make_shared<jobq::FileJobStorage>(torn.journal())
    );
    check(manager.totalJobs() == 1 && queue2.size() == 1,
          "Unterminated tail record from a torn write is ignored");
}

void testNextJobId() {
    std::cout << "\n--- Test I: Next Job ID After Recovery ---\n";

    {
        jobq::JobQueue queue;
        DoubleStorage storage;
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        jobq::JobManager manager(queue, shared);
        check(manager.submit("T", "p")->getId() == 1,
              "Empty storage starts IDs at 1");
    }

    {
        jobq::JobQueue queue;
        DoubleStorage storage;
        storage.records = {makeRecord(1, JobStatus::Completed, 1)};
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        jobq::JobManager manager(queue, shared);
        check(manager.submit("T", "p")->getId() == 2,
              "Single record 1 gives next ID 2");
    }

    {
        jobq::JobQueue queue;
        DoubleStorage storage;
        storage.records = {
            makeRecord(7, JobStatus::Completed, 1),
            makeRecord(3, JobStatus::Cancelled, 0),
            makeRecord(20, JobStatus::Pending, 0),
        };
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        jobq::JobManager manager(queue, shared);
        const auto first = manager.submit("T", "p");
        const auto second = manager.submit("T", "p");
        check(first->getId() == 21 && second->getId() == 22,
              "Unsorted, gapped records give IDs above the maximum");
        check(manager.getJob(7) && manager.getJob(3) && manager.getJob(20),
              "Restored jobs not displaced by new IDs");
    }

    {
        jobq::JobQueue queue;
        DoubleStorage storage;
        const auto high = std::numeric_limits<std::uint64_t>::max() - 1;
        storage.records = {makeRecord(high, JobStatus::Completed, 1)};
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        jobq::JobManager manager(queue, shared);
        check(manager.submit("T", "p")->getId() == high + 1,
              "Highest valid ID leaves one more usable ID");
    }

    {
        jobq::JobQueue queue;
        DoubleStorage storage;
        storage.records = {makeRecord(
            std::numeric_limits<std::uint64_t>::max(),
            JobStatus::Completed,
            1
        )};
        auto shared = std::shared_ptr<DoubleStorage>(
            &storage, [](DoubleStorage*) {}
        );
        bool overflow = false;
        try {
            jobq::JobManager manager(queue, shared);
        } catch (const std::overflow_error&) {
            overflow = true;
        }
        check(overflow, "Maximum uint64 ID rejected as an overflow");
    }
}

void testStorageReadFailure() {
    std::cout << "\n--- Test J: Storage Read Failure ---\n";

    DoubleStorage storage;
    storage.failLoad = true;
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );
    jobq::JobQueue queue;

    check(throwsWith<std::runtime_error>(
              [&] { jobq::JobManager manager(queue, shared); },
              "simulated read failure"),
          "Construction reports the storage read failure");
    check(queue.empty(), "Nothing queued after a failed load");

    TempDir dir("j");
    std::filesystem::create_directories(dir.journal());
    check(throwsWith<std::exception>(
              [&] {
                  jobq::JobQueue q;
                  jobq::JobManager manager(
                      q,
                      std::make_shared<jobq::FileJobStorage>(dir.journal())
                  );
              },
              ""),
          "Unreadable journal path is an error, not an empty store");

    TempDir missing("j_missing");
    jobq::JobQueue q2;
    jobq::JobManager manager(
        q2, std::make_shared<jobq::FileJobStorage>(missing.journal())
    );
    check(manager.totalJobs() == 0, "Missing journal file is an empty store");
}

void testStorageWriteFailure() {
    std::cout << "\n--- Test K: Storage Write Failure ---\n";

    DoubleStorage storage;
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );
    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, shared, metrics);

    storage.failSave = true;
    check(throwsWith<std::runtime_error>(
              [&] { manager.submit("T", "p"); }, "simulated write failure"),
          "Submission failure is reported");
    check(manager.totalJobs() == 0, "Registry holds no phantom job");
    check(queue.empty(), "Failed submission not queued");
    check(metrics.snapshot().submittedJobs == 0,
          "Failed submission not counted");

    storage.failSave = false;
    const auto job = manager.submit("T", "p");
    check(job && manager.totalJobs() == 1 && queue.size() == 1,
          "Submission works again once storage recovers");

    storage.failSave = true;
    const auto popped = queue.tryPop();
    check(throwsWith<std::runtime_error>(
              [&] { popped->markRunning(); }, "simulated write failure"),
          "State transition surfaces the persistence failure");
    check(popped->getStatus() == JobStatus::Pending &&
              popped->getAttemptsMade() == 0,
          "Job state unchanged when its snapshot could not be saved");
}

void testRecoveryQueueRejection() {
    std::cout << "\n--- Test L: Recovery Queue Rejection ---\n";

    DoubleStorage storage;
    storage.records = {makeRecord(1, JobStatus::Pending, 0)};
    auto shared = std::shared_ptr<DoubleStorage>(
        &storage, [](DoubleStorage*) {}
    );

    jobq::JobQueue queue;
    queue.close();

    check(throwsWith<std::runtime_error>(
              [&] { jobq::JobManager manager(queue, shared); },
              "Failed to enqueue recovered job 1"),
          "Rejected recovery enqueue is reported with the job ID");
    check(queue.empty(), "Rejected job not inserted into the closed queue");
    check(storage.saved.empty(),
          "Stored state stays Pending for the next recovery attempt");

    DoubleStorage retryStorage;
    retryStorage.records = {makeRecord(2, JobStatus::Failed, 1, instantRetry(2))};
    auto retryShared = std::shared_ptr<DoubleStorage>(
        &retryStorage, [](DoubleStorage*) {}
    );
    jobq::JobQueue closedQueue;
    closedQueue.close();
    jobq::JobManager manager(closedQueue, retryShared);
    check(closedQueue.size() == 1,
          "Recovered retry still accepted by a closed queue (drain policy)");
}

void testRepeatedRestart() {
    std::cout << "\n--- Test M: Repeated Restart and Recovery ---\n";

    TempDir dir("m");

    {
        jobq::JobQueue queue;
        jobq::JobManager manager(
            queue, std::make_shared<jobq::FileJobStorage>(dir.journal())
        );
        const auto done = manager.submit("T", "done");
        manager.submit("T", "waiting");
        const auto interrupted =
            manager.submit("T", "interrupted", JobPriority::Normal, slowRetry(2));

        const auto first = queue.tryPop();
        const auto second = queue.tryPop();
        const auto third = queue.tryPop();
        (void)first;
        (void)second;
        (void)third;

        done->markRunning();
        done->markCompleted();
        interrupted->markRunning();
    }

    std::uint64_t expectedNewId = 4;
    for (int cycle = 1; cycle <= 3; ++cycle) {
        jobq::JobQueue queue;
        jobq::JobMetrics metrics;
        jobq::JobManager manager(
            queue,
            std::make_shared<jobq::FileJobStorage>(dir.journal()),
            metrics
        );

        const std::string label = "cycle " + std::to_string(cycle) + ": ";
        const std::size_t expectedJobs = 3 + (cycle - 1);

        check(manager.totalJobs() == expectedJobs,
              label + "no duplicate records appear");
        check(manager.getStatus(1) == JobStatus::Completed,
              label + "completed job stays Completed");
        check(manager.getStatus(2) == JobStatus::Pending &&
                  manager.getJob(2)->getAttemptsMade() == 0,
              label + "pending job restored untouched");
        check(manager.getStatus(3) == JobStatus::Pending &&
                  manager.getJob(3)->getAttemptsMade() == 1,
              label + "interrupted job keeps its single attempt");
        check(queue.size() == expectedJobs - 1,
              label + "everything except the completed job is queued");
        check(metrics.snapshot().submittedJobs == 0,
              label + "recovery does not count submissions");

        const auto fresh = manager.submit("T", "new");
        check(fresh->getId() == expectedNewId,
              label + "new job receives the next unique ID");
        ++expectedNewId;
    }

    jobq::FileJobStorage verify(dir.journal());
    check(verify.loadAll().size() == 6,
          "Journal resolves to one record per job ID after all restarts");
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobQ Recovery Hardening Tests\n";
    std::cout << "========================================\n";

    try {
        testMultiplePendingRecovery();
        testInterruptedRunningRecovery();
        testRetryableFailedRecovery();
        testExhaustedRetryRecovery();
        testCompletedRecovery();
        testCancelledRecovery();
        testDuplicateIds();
        testInvalidRecords();
        testNextJobId();
        testStorageReadFailure();
        testStorageWriteFailure();
        testRecoveryQueueRejection();
        testRepeatedRestart();
    } catch (const std::exception& error) {
        ++testsFailed;
        std::cout << "[FAIL] Unexpected exception: " << error.what() << '\n';
    }

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
