
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "storage/JobStorage.h"

namespace {

using namespace std::chrono_literals;

int testsRun = 0;
int testsFailed = 0;

const std::filesystem::path& testRoot() {
    static const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "jobq_manager_persistence_tests";

    return root;
}

std::filesystem::path journalPath(const std::string& name) {
    return testRoot() / name / "jobs.journal";
}

void resetStorage() {
    std::filesystem::remove_all(testRoot());
}

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

const jobq::JobRecord* findRecord(
    const std::vector<jobq::JobRecord>& records,
    std::uint64_t id
) {
    const auto iterator = std::find_if(
        records.begin(),
        records.end(),
        [id](const jobq::JobRecord& record) {
            return record.id == id;
        }
    );

    return iterator == records.end() ? nullptr : &*iterator;
}

class FailingStorage final : public jobq::JobStorage {
public:
    void save(const jobq::JobRecord&) override {
        throw std::runtime_error(
            "Simulated storage failure"
        );
    }

    std::vector<jobq::JobRecord> loadAll() override {
        return {};
    }
};

void testPersistedLifecycleAndRestart() {
    std::cout << "\n--- Persisted Lifecycle and Restart ---\n";

    resetStorage();

    const auto path = journalPath("completed");

    std::uint64_t jobId = 0;

    {
        jobq::JobQueue queue;

        auto storage = std::make_shared<jobq::FileJobStorage>(path);

        jobq::JobManager manager(queue, storage);

        auto job = manager.submit(
            "REPORT",
            "persistent-report",
            jobq::JobPriority::High,
            jobq::RetryPolicy{
                2,
                25ms,
                2.0,
                200ms
            }
        );

        jobId = job->getId();

        check(
            storage->loadAll().size() == 1,
            "Submission persists the initial PENDING record"
        );

        check(
            queue.tryPop() == job,
            "Submitted job is available in the queue"
        );

        job->markRunning();

        const auto runningRecords = storage->loadAll();
        const auto* running = findRecord(runningRecords, jobId);

        check(
            running != nullptr &&
                running->status == jobq::JobStatus::Running &&
                running->attemptsMade == 1,
            "RUNNING transition persists status and attempt count"
        );

        job->markCompleted();

        const auto completedRecords = storage->loadAll();
        const auto* completed =
            findRecord(completedRecords, jobId);

        check(
            completed != nullptr &&
                completed->status == jobq::JobStatus::Completed,
            "COMPLETED transition persists the final status"
        );
    }

    // A fresh storage and manager simulate application restart.
    {
        jobq::JobQueue restartedQueue;

        auto restartedStorage =
            std::make_shared<jobq::FileJobStorage>(path);

        jobq::JobManager restartedManager(
            restartedQueue,
            restartedStorage
        );

        const auto restored = restartedManager.getJob(jobId);

        check(
            restored != nullptr,
            "A fresh manager restores the saved job record"
        );

        check(
            restored &&
                restored->getStatus() == jobq::JobStatus::Completed,
            "Restored job keeps its COMPLETED status"
        );

        check(
            restored &&
                restored->getAttemptsMade() == 1,
            "Restored job retains its attempt count"
        );

        check(
            restored &&
                restored->getPriority() == jobq::JobPriority::High &&
                restored->getRetryPolicy().maxRetries == 2 &&
                restored->getRetryPolicy().initialDelay == 25ms,
            "Restored job retains priority and retry policy"
        );

        check(
            restartedManager.totalJobs() == 1,
            "Registry contains the restored record"
        );

        check(
            restartedQueue.empty(),
            "Commit 3 restores registry records without re-enqueuing jobs"
        );
    }
}

void testPendingRetryStateIsPersisted() {
    std::cout << "\n--- Persisted Retry Preparation ---\n";

    resetStorage();

    const auto path = journalPath("retry-pending");
    std::uint64_t jobId = 0;

    {
        jobq::JobQueue queue;

        auto storage = std::make_shared<jobq::FileJobStorage>(path);
        jobq::JobManager manager(queue, storage);

        auto job = manager.submit(
            "EMAIL",
            "retry-after-restart",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                25ms,
                2.0,
                200ms
            }
        );

        jobId = job->getId();

        queue.tryPop();

        job->markRunning();
        job->markFailed();

        check(
            manager.getStatus(jobId) == jobq::JobStatus::Failed,
            "Failed state is visible through JobManager"
        );

        check(
            job->prepareForRetry(),
            "Retry preparation succeeds within policy"
        );

        const auto records = storage->loadAll();
        const auto* record = findRecord(records, jobId);

        check(
            record != nullptr &&
                record->status == jobq::JobStatus::Pending &&
                record->attemptsMade == 1,
            "Retry preparation persists PENDING with prior attempt count"
        );
    }

    {
        jobq::JobQueue restartedQueue;

        auto storage = std::make_shared<jobq::FileJobStorage>(path);

        jobq::JobManager restartedManager(
            restartedQueue,
            storage
        );

        auto restored = restartedManager.getJob(jobId);

        check(
            restored &&
                restored->getStatus() == jobq::JobStatus::Pending,
            "Restarted manager restores the prepared retry"
        );

        check(
            restored &&
                restored->getAttemptsMade() == 1,
            "Restarted retry retains previous attempts"
        );

        check(
            restartedQueue.empty(),
            "Pending retry is not scheduled until the recovery phase"
        );
    }
}

void testRejectedSubmissionPersistsCancellation() {
    std::cout << "\n--- Rejected Submission Persistence ---\n";

    resetStorage();

    const auto path = journalPath("rejected");

    {
        jobq::JobQueue queue;
        queue.close();

        auto storage = std::make_shared<jobq::FileJobStorage>(path);
        jobq::JobManager manager(queue, storage);

        bool rejected = false;

        try {
            manager.submit(
                "EMAIL",
                "queue-closed",
                jobq::JobPriority::Normal,
                jobq::RetryPolicy{}
            );
        } catch (const std::runtime_error&) {
            rejected = true;
        }

        check(
            rejected,
            "Closed queue rejects a persistent submission"
        );

        check(
            manager.totalJobs() == 0,
            "Rejected job is removed from the current registry"
        );

        const auto records = storage->loadAll();

        check(
            records.size() == 1 &&
                records.front().status == jobq::JobStatus::Cancelled,
            "Rejected submission leaves a CANCELLED storage snapshot"
        );
    }
}

void testStorageFailureRejectsSubmission() {
    std::cout << "\n--- Storage Failure Handling ---\n";

    jobq::JobQueue queue;

    auto storage = std::make_shared<FailingStorage>();

    jobq::JobManager manager(queue, storage);

    bool submissionFailed = false;

    try {
        manager.submit(
            "EMAIL",
            "simulated-storage-failure"
        );
    } catch (const std::runtime_error&) {
        submissionFailed = true;
    }

    check(
        submissionFailed,
        "Storage failure is surfaced to the caller"
    );

    check(
        manager.totalJobs() == 0,
        "Failed persistence does not leave a registry entry"
    );

    check(
        queue.empty(),
        "Job is not queued when its initial snapshot cannot be saved"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobManager Persistence Tests       \n";
    std::cout << "========================================\n";

    try {
        testPersistedLifecycleAndRestart();
        testPendingRetryStateIsPersisted();
        testRejectedSubmissionPersistsCancellation();
        testStorageFailureRejectsSubmission();
    } catch (const std::exception& exception) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected exception: "
            << exception.what()
            << '\n';
    } catch (...) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected non-standard exception\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';

    if (testsFailed == 0) {
        std::cout << "Result: ALL TESTS PASSED\n";
    } else {
        std::cout << "Result: TESTS FAILED\n";
    }

    std::cout << "========================================\n";

    resetStorage();

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
