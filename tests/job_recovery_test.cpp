
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
#include "storage/JobRecord.h"

namespace {

using namespace std::chrono_literals;

int testsRun = 0;
int testsFailed = 0;

const std::filesystem::path& testRoot() {
    static const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "jobq_crash_recovery_tests";

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

jobq::JobRecord makeRecord(
    uint64_t id,
    std::string type,
    std::string payload,
    jobq::JobStatus status,
    std::size_t attemptsMade,
    jobq::JobPriority priority = jobq::JobPriority::Normal,
    jobq::RetryPolicy retryPolicy = {}
) {
    jobq::JobRecord record;

    record.id = id;
    record.type = std::move(type);
    record.payload = std::move(payload);
    record.status = status;
    record.attemptsMade = attemptsMade;
    record.priority = priority;
    record.retryPolicy = retryPolicy;

    return record;
}

const jobq::JobRecord* findRecord(
    const std::vector<jobq::JobRecord>& records,
    uint64_t id
) {
    const auto it = std::find_if(
        records.begin(),
        records.end(),
        [id](const jobq::JobRecord& record) {
            return record.id == id;
        }
    );

    return it == records.end() ? nullptr : &*it;
}

void testPendingJobsAreRestored() {
    std::cout << "\n--- Pending Job Recovery ---\n";

    resetStorage();

    const auto path = journalPath("pending");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                11,
                "EMAIL",
                "low-pending",
                jobq::JobStatus::Pending,
                0,
                jobq::JobPriority::Low
            )
        );

        storage.save(
            makeRecord(
                22,
                "REPORT",
                "high-pending",
                jobq::JobStatus::Pending,
                0,
                jobq::JobPriority::High
            )
        );
    }

    jobq::JobQueue queue;
    auto storage = std::make_shared<jobq::FileJobStorage>(path);
    jobq::JobManager manager(queue, storage);

    check(
        manager.totalJobs() == 2,
        "All pending jobs are restored into the registry"
    );

    check(
        queue.size() == 2,
        "All pending jobs are restored into the queue"
    );

    auto first = queue.tryPop();
    auto second = queue.tryPop();

    check(
        first && first->getId() == 22,
        "HIGH priority pending job is dequeued first"
    );

    check(
        second && second->getId() == 11,
        "LOW priority pending job is dequeued second"
    );

    check(
        manager.getStatus(11) == jobq::JobStatus::Pending &&
            manager.getStatus(22) == jobq::JobStatus::Pending,
        "Restoring pending jobs does not increment attempts"
    );

    auto nextJob = manager.createJob(
        "EMAIL",
        "new-after-recovery"
    );

    check(
        nextJob->getId() > 22,
        "New IDs continue above the highest recovered ID"
    );
}

void testRunningJobWithRetryBudgetIsRecovered() {
    std::cout << "\n--- Interrupted Running Job Recovery ---\n";

    resetStorage();

    const auto path = journalPath("running-retry");

    const jobq::RetryPolicy policy{
        2,
        0ms,
        2.0,
        0ms
    };

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                31,
                "REPORT",
                "interrupted-report",
                jobq::JobStatus::Running,
                1,
                jobq::JobPriority::High,
                policy
            )
        );
    }

    jobq::JobQueue queue;
    auto storage = std::make_shared<jobq::FileJobStorage>(path);
    jobq::JobManager manager(queue, storage);

    const auto recovered = manager.getJob(31);

    check(
        recovered != nullptr,
        "Interrupted job is restored in the registry"
    );

    check(
        recovered &&
            recovered->getStatus() == jobq::JobStatus::Pending,
        "Interrupted job with retry budget returns to PENDING"
    );

    check(
        recovered && recovered->getAttemptsMade() == 1,
        "Interrupted attempt count is preserved"
    );

    check(
        queue.size() == 1,
        "Recoverable interrupted job is scheduled"
    );

    check(
        queue.tryPop() == recovered,
        "Queue returns the recovered job"
    );

    const auto records = storage->loadAll();
    const auto* persisted = findRecord(records, 31);

    check(
        persisted &&
            persisted->status == jobq::JobStatus::Pending,
        "Recovery persists the normalized PENDING state"
    );
}

void testRunningJobWithoutRetryBudgetStaysFailed() {
    std::cout << "\n--- Interrupted Job With Exhausted Retry Budget ---\n";

    resetStorage();

    const auto path = journalPath("running-exhausted");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                41,
                "EMAIL",
                "interrupted-final-attempt",
                jobq::JobStatus::Running,
                1,
                jobq::JobPriority::Normal,
                jobq::RetryPolicy{0, 0ms, 2.0, 0ms}
            )
        );
    }

    jobq::JobQueue queue;
    auto storage = std::make_shared<jobq::FileJobStorage>(path);
    jobq::JobManager manager(queue, storage);

    check(
        manager.getStatus(41) == jobq::JobStatus::Failed,
        "Interrupted job becomes FAILED when no retries remain"
    );

    check(
        queue.empty(),
        "Exhausted interrupted job is not re-enqueued"
    );

    const auto records = storage->loadAll();
    const auto* persisted = findRecord(records, 41);

    check(
        persisted &&
            persisted->status == jobq::JobStatus::Failed,
        "The failure caused by interruption is persisted"
    );
}

void testRetryableFailedJobIsRecovered() {
    std::cout << "\n--- Previously Failed Job Recovery ---\n";

    resetStorage();

    const auto path = journalPath("failed-retry");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                51,
                "REPORT",
                "retryable-failure",
                jobq::JobStatus::Failed,
                1,
                jobq::JobPriority::Normal,
                jobq::RetryPolicy{2, 0ms, 2.0, 0ms}
            )
        );
    }

    jobq::JobQueue queue;
    auto storage = std::make_shared<jobq::FileJobStorage>(path);
    jobq::JobManager manager(queue, storage);

    const auto recovered = manager.getJob(51);

    check(
        recovered &&
            recovered->getStatus() == jobq::JobStatus::Pending,
        "Retryable FAILED job is prepared for another attempt"
    );

    check(
        recovered && recovered->getAttemptsMade() == 1,
        "Prepared job keeps its previous attempt count"
    );

    check(
        queue.tryPop() == recovered,
        "Retryable failed job is restored into the queue"
    );
}

void testPendingRetrySurvivesRestart() {
    std::cout << "\n--- Pending Retry Recovery ---\n";

    resetStorage();

    const auto path = journalPath("pending-retry");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                61,
                "EMAIL",
                "prepared-retry",
                jobq::JobStatus::Pending,
                1,
                jobq::JobPriority::High,
                jobq::RetryPolicy{2, 0ms, 2.0, 0ms}
            )
        );
    }

    jobq::JobQueue queue;
    auto storage = std::make_shared<jobq::FileJobStorage>(path);
    jobq::JobManager manager(queue, storage);

    const auto recovered = manager.getJob(61);

    check(
        recovered &&
            recovered->getStatus() == jobq::JobStatus::Pending,
        "Already prepared retry remains PENDING"
    );

    check(
        recovered && recovered->getAttemptsMade() == 1,
        "Pending retry retains its attempt count"
    );

    check(
        queue.tryPop() == recovered,
        "Prepared retry is scheduled again after restart"
    );
}

void testTerminalJobsAreNotReenqueued() {
    std::cout << "\n--- Terminal Job Recovery ---\n";

    resetStorage();

    const auto path = journalPath("terminal");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                71,
                "EMAIL",
                "completed-job",
                jobq::JobStatus::Completed,
                1
            )
        );

        storage.save(
            makeRecord(
                72,
                "REPORT",
                "cancelled-job",
                jobq::JobStatus::Cancelled,
                0
            )
        );

        storage.save(
            makeRecord(
                73,
                "REPORT",
                "exhausted-failed-job",
                jobq::JobStatus::Failed,
                2,
                jobq::JobPriority::Normal,
                jobq::RetryPolicy{1, 0ms, 2.0, 0ms}
            )
        );
    }

    jobq::JobQueue queue;
    auto storage = std::make_shared<jobq::FileJobStorage>(path);
    jobq::JobManager manager(queue, storage);

    check(
        manager.getStatus(71) == jobq::JobStatus::Completed,
        "COMPLETED job is restored as terminal"
    );

    check(
        manager.getStatus(72) == jobq::JobStatus::Cancelled,
        "CANCELLED job is restored as terminal"
    );

    check(
        manager.getStatus(73) == jobq::JobStatus::Failed,
        "Exhausted FAILED job remains terminal"
    );

    check(
        queue.empty(),
        "Terminal jobs are not added to the queue"
    );
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "          Job Recovery Tests            \n";
    std::cout << "========================================\n";

    try {
        testPendingJobsAreRestored();
        testRunningJobWithRetryBudgetIsRecovered();
        testRunningJobWithoutRetryBudgetStaysFailed();
        testRetryableFailedJobIsRecovered();
        testPendingRetrySurvivesRestart();
        testTerminalJobsAreNotReenqueued();
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
