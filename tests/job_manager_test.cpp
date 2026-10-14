#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"

namespace {

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

void testCreateAndLookup() {
    std::cout << "\n--- Job Creation and Lookup ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto firstJob = manager.createJob(
        "EMAIL",
        "first-test-job"
    );

    auto secondJob = manager.createJob(
        "REPORT",
        "second-test-job"
    );

    const auto foundJob = manager.getJob(firstJob->getId());
    const auto status = manager.getStatus(firstJob->getId());

    check(
        firstJob != nullptr && secondJob != nullptr,
        "JobManager creates jobs"
    );

    check(
        firstJob->getId() != secondJob->getId(),
        "Generated job IDs are unique"
    );

    check(
        foundJob == firstJob,
        "Registry returns the correct job"
    );

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Pending,
        "New job starts in PENDING status"
    );

    check(
        manager.totalJobs() == 2,
        "Registry tracks both created jobs"
    );

    check(
        queue.empty(),
        "createJob does not enqueue the job"
    );

    check(
        manager.getJob(0) == nullptr,
        "Unknown job ID returns nullptr"
    );

    check(
        !manager.getStatus(0).has_value(),
        "Unknown job ID has no status"
    );
}

void testSubmissionAndLifecycleTracking() {
    std::cout << "\n--- Submission and Lifecycle Tracking ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto submittedJob = manager.submit(
        "EMAIL",
        "lifecycle-test-job"
    );

    check(
        manager.totalJobs() == 1,
        "Submitted job is stored in the registry"
    );

    check(
        queue.size() == 1,
        "Submitted job is added to the queue"
    );

    const auto queuedJob = queue.tryPop();

    check(
        queuedJob == submittedJob,
        "Queue returns the submitted job"
    );

    auto status = manager.getStatus(submittedJob->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Pending,
        "Submitted job is initially PENDING"
    );

    submittedJob->markRunning();

    status = manager.getStatus(submittedJob->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Running,
        "Manager reflects the RUNNING status"
    );

    submittedJob->markCompleted();

    status = manager.getStatus(submittedJob->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Completed,
        "Manager reflects the COMPLETED status"
    );
}

void testFailedJobTracking() {
    std::cout << "\n--- Failed Job Tracking ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto job = manager.submit(
        "FAIL",
        "failed-job-test"
    );

    const auto queuedJob = queue.tryPop();

    check(
        queuedJob == job,
        "Failed-job test retrieves the submitted job"
    );

    job->markRunning();
    job->markFailed();

    const auto status = manager.getStatus(job->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Failed,
        "Manager reflects the FAILED status"
    );

    check(
        manager.getJob(job->getId()) == job,
        "Failed job remains available in the registry"
    );
}

void testCancelledJobTracking() {
    std::cout << "\n--- Cancelled Job Tracking ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto job = manager.createJob(
        "REPORT",
        "cancelled-job-test"
    );

    job->markCancelled();

    const auto status = manager.getStatus(job->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Cancelled,
        "Manager reflects the CANCELLED status"
    );
}

void testClosedQueueRollback() {
    std::cout << "\n--- Closed Queue Submission Rollback ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto existingJob = manager.createJob(
        "EMAIL",
        "existing-registry-job"
    );

    const auto initialCount = manager.totalJobs();

    queue.close();

    bool submissionRejected = false;

    try {
        manager.submit(
            "EMAIL",
            "should-not-be-submitted"
        );
    } catch (const std::runtime_error&) {
        submissionRejected = true;
    }

    check(
        submissionRejected,
        "Submission to a closed queue throws an error"
    );

    check(
        manager.totalJobs() == initialCount,
        "Rejected submission is removed from the registry"
    );

    check(
        manager.getJob(existingJob->getId()) == existingJob,
        "Rollback preserves previously registered jobs"
    );

    check(
        queue.empty(),
        "Rejected job is not added to the closed queue"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "       JobManager Automated Tests       \n";
    std::cout << "========================================\n";

    try {
        testCreateAndLookup();
        testSubmissionAndLifecycleTracking();
        testFailedJobTracking();
        testCancelledJobTracking();
        testClosedQueueRollback();
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

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}