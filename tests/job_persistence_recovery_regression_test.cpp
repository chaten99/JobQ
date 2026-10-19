
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"
#include "storage/JobStorage.h"

namespace {

int testsRun = 0;
int testsFailed = 0;

const std::filesystem::path& testRoot() {
    static const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "jobq_persistence_regression_tests";

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
    std::uint64_t id,
    std::string type,
    std::string payload,
    jobq::JobStatus status = jobq::JobStatus::Pending,
    std::size_t attempts = 0,
    jobq::RetryPolicy policy = {}
) {
    jobq::JobRecord record;

    record.id = id;
    record.type = std::move(type);
    record.payload = std::move(payload);
    record.status = status;
    record.attemptsMade = attempts;
    record.priority = jobq::JobPriority::Normal;
    record.retryPolicy = policy;

    return record;
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

    return iterator == records.end()
        ? nullptr
        : &*iterator;
}

class TestStorage final : public jobq::JobStorage {
public:
    explicit TestStorage(
        std::vector<jobq::JobRecord> initialRecords = {}
    )
        : records_(std::move(initialRecords)) {}

    void save(const jobq::JobRecord& record) override {
        if (failNextSave_) {
            failNextSave_ = false;

            throw std::runtime_error(
                "Simulated persistence failure"
            );
        }

        const auto iterator = std::find_if(
            records_.begin(),
            records_.end(),
            [&record](const jobq::JobRecord& existing) {
                return existing.id == record.id;
            }
        );

        if (iterator == records_.end()) {
            records_.push_back(record);
        } else {
            *iterator = record;
        }
    }

    std::vector<jobq::JobRecord> loadAll() override {
        return records_;
    }

    void failNextSave() {
        failNextSave_ = true;
    }

private:
    std::vector<jobq::JobRecord> records_;
    bool failNextSave_{false};
};

void testRecoveryDoesNotExceedRetryBudget() {
    std::cout << "\n--- Recovery Retry Exhaustion ---\n";

    resetStorage();

    const auto path = journalPath("exhausted");

    const jobq::RetryPolicy policy{
        1,
        std::chrono::milliseconds{0},
        2.0,
        std::chrono::milliseconds{0}
    };

    {
        jobq::FileJobStorage storage(path);
        storage.save(
            makeRecord(
                101,
                "EMAIL",
                "interrupted-final-attempt",
                jobq::JobStatus::Running,
                2,
                policy
            )
        );
    }

    jobq::JobQueue queue;

    auto storage =
        std::make_shared<jobq::FileJobStorage>(path);

    jobq::JobManager manager(queue, storage);

    const auto restored = manager.getJob(101);

    check(
        restored != nullptr,
        "Exhausted job remains available in the registry"
    );

    check(
        restored &&
            restored->getStatus() == jobq::JobStatus::Failed,
        "Interrupted job with exhausted retries stays FAILED"
    );

    check(
        restored && restored->getAttemptsMade() == 2,
        "Recovery preserves the actual attempt count"
    );

    check(
        queue.empty(),
        "Exhausted job is not re-enqueued"
    );

    const auto persistedRecords = storage->loadAll();
    const auto* persisted = findRecord(persistedRecords, 101);

    check(
        persisted &&
            persisted->status == jobq::JobStatus::Failed,
        "Failure is persisted during recovery"
    );
}

void testDuplicateIdsPreventRecovery() {
    std::cout << "\n--- Duplicate ID Protection ---\n";

    auto first = makeRecord(
        201,
        "EMAIL",
        "first-version"
    );

    auto duplicate = makeRecord(
        201,
        "REPORT",
        "duplicate-version"
    );

    auto storage = std::make_shared<TestStorage>(
        std::vector<jobq::JobRecord>{
            first,
            duplicate
        }
    );

    jobq::JobQueue queue;

    bool duplicateRejected = false;

    try {
        jobq::JobManager manager(queue, storage);
    } catch (const std::runtime_error&) {
        duplicateRejected = true;
    }

    check(
        duplicateRejected,
        "Recovery rejects duplicate job IDs from storage"
    );

    check(
        queue.empty(),
        "No jobs are scheduled after duplicate IDs are detected"
    );
}

void testCorruptedJournalPreventsRecovery() {
    std::cout << "\n--- Corrupted Journal Protection ---\n";

    resetStorage();

    const auto path = journalPath("corrupt");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                301,
                "EMAIL",
                "valid-pending-job"
            )
        );
    }

    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::app
        );

        output << "INVALID_COMPLETE_RECORD\n";
    }

    jobq::JobQueue queue;

    auto storage =
        std::make_shared<jobq::FileJobStorage>(path);

    bool corruptionReported = false;

    try {
        jobq::JobManager manager(queue, storage);
    } catch (const std::runtime_error&) {
        corruptionReported = true;
    }

    check(
        corruptionReported,
        "Complete corrupted journal record causes a clear error"
    );

    check(
        queue.empty(),
        "Jobs are not scheduled from a journal that failed validation"
    );
}

void testPersistenceFailureDoesNotAdvanceJobState() {
    std::cout << "\n--- Lifecycle Persistence Failure ---\n";

    jobq::JobQueue queue;
    auto storage = std::make_shared<TestStorage>();

    jobq::JobManager manager(queue, storage);

    auto job = manager.submit(
        "EMAIL",
        "state-must-not-advance",
        jobq::JobPriority::Normal,
        jobq::RetryPolicy{}
    );

    check(
        queue.tryPop() == job,
        "Submitted job can be retrieved for transition testing"
    );

    storage->failNextSave();

    bool transitionFailed = false;

    try {
        job->markRunning();
    } catch (const std::runtime_error&) {
        transitionFailed = true;
    }

    check(
        transitionFailed,
        "Persistence exception is reported to the caller"
    );

    check(
        job->getStatus() == jobq::JobStatus::Pending,
        "Failed persistence does not advance in-memory status"
    );

    check(
        job->getAttemptsMade() == 0,
        "Failed persistence does not increment attempt count"
    );

    const auto records = storage->loadAll();
    const auto* persisted = findRecord(records, job->getId());

    check(
        persisted &&
            persisted->status == jobq::JobStatus::Pending &&
            persisted->attemptsMade == 0,
        "Stored snapshot remains consistent after persistence failure"
    );
}

void testCompletedJobsAreNotRecoveredForExecution() {
    std::cout << "\n--- Terminal Job Recovery Idempotence ---\n";

    resetStorage();

    const auto path = journalPath("terminal");

    {
        jobq::FileJobStorage storage(path);

        storage.save(
            makeRecord(
                401,
                "EMAIL",
                "already-completed",
                jobq::JobStatus::Completed,
                1
            )
        );

        storage.save(
            makeRecord(
                402,
                "REPORT",
                "cancelled-job",
                jobq::JobStatus::Cancelled,
                0
            )
        );
    }

    // Repeat recovery using fresh queues and fresh managers.
    for (int iteration = 0; iteration < 2; ++iteration) {
        jobq::JobQueue queue;

        auto storage =
            std::make_shared<jobq::FileJobStorage>(path);

        jobq::JobManager manager(queue, storage);

        check(
            manager.getStatus(401) == jobq::JobStatus::Completed,
            "Completed job remains terminal across restarts"
        );

        check(
            manager.getStatus(402) == jobq::JobStatus::Cancelled,
            "Cancelled job remains terminal across restarts"
        );

        check(
            queue.empty(),
            "Repeated recovery does not re-enqueue terminal jobs"
        );
    }
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " Persistence Recovery Regression Tests  \n";
    std::cout << "========================================\n";

    try {
        testRecoveryDoesNotExceedRetryBudget();
        testDuplicateIdsPreventRecovery();
        testCorruptedJournalPreventsRecovery();
        testPersistenceFailureDoesNotAdvanceJobState();
        testCompletedJobsAreNotRecoveredForExecution();
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
