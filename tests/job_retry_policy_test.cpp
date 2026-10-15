#include <cstdlib>
#include <iostream>
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

void testDefaultRetryPolicy() {
    std::cout << "\n--- Default Retry Policy ---\n";

    jobq::Job job(
        1,
        "EMAIL",
        "default-retry-policy"
    );

    check(
        job.getRetryPolicy().maxRetries == 0,
        "Default retry policy allows zero retries"
    );

    check(
        job.getAttemptsMade() == 0,
        "New job starts with zero attempts"
    );

    check(
        !job.canRetry(),
        "Pending job cannot be retried"
    );

    job.markRunning();

    check(
        job.getAttemptsMade() == 1,
        "First transition to RUNNING counts one attempt"
    );

    check(
        !job.canRetry(),
        "Running job cannot be retried"
    );

    job.markFailed();

    check(
        !job.canRetry(),
        "Default policy does not permit another attempt"
    );

    check(
        !job.prepareForRetry(),
        "Retry preparation fails when retry budget is exhausted"
    );

    check(
        job.getStatus() == jobq::JobStatus::Failed,
        "Exhausted job remains FAILED"
    );
}

void testRetryLifecycle() {
    std::cout << "\n--- Retry Lifecycle ---\n";

    jobq::Job job(
        2,
        "REPORT",
        "retry-lifecycle",
        jobq::JobPriority::High,
        jobq::RetryPolicy{2}
    );

    check(
        job.getRetryPolicy().maxRetries == 2,
        "Configured retry limit is preserved"
    );

    // Attempt 1.
    job.markRunning();

    check(
        job.getAttemptsMade() == 1,
        "First execution is recorded"
    );

    job.markFailed();

    check(
        job.canRetry(),
        "Retry is available after first failure"
    );

    check(
        job.prepareForRetry(),
        "First retry can be prepared"
    );

    check(
        job.getStatus() == jobq::JobStatus::Pending,
        "Prepared retry returns job to PENDING"
    );

    // Attempt 2.
    job.markRunning();

    check(
        job.getAttemptsMade() == 2,
        "Second execution is recorded"
    );

    check(
        !job.canRetry(),
        "Running job is not yet eligible for retry"
    );

    job.markFailed();

    check(
        job.canRetry(),
        "Second failure still has one retry remaining"
    );

    check(
        job.prepareForRetry(),
        "Second retry can be prepared"
    );

    // Attempt 3: initial execution plus two retries.
    job.markRunning();

    check(
        job.getAttemptsMade() == 3,
        "Third execution is recorded"
    );

    job.markFailed();

    check(
        !job.canRetry(),
        "Retry is unavailable after maxRetries is exhausted"
    );

    check(
        !job.prepareForRetry(),
        "Exhausted retry policy rejects another retry"
    );

    check(
        job.getStatus() == jobq::JobStatus::Failed,
        "Job remains FAILED after exhausting retries"
    );

    check(
        job.getAttemptsMade() == 3,
        "Rejected retry does not increment attempt count"
    );
}

void testRetryPreparationValidation() {
    std::cout << "\n--- Retry Preparation Validation ---\n";

    jobq::Job job(
        3,
        "EMAIL",
        "invalid-retry-transition",
        jobq::JobPriority::Normal,
        jobq::RetryPolicy{1}
    );

    bool pendingRejected = false;

    try {
        job.prepareForRetry();
    } catch (const std::logic_error&) {
        pendingRejected = true;
    }

    check(
        pendingRejected,
        "Retry preparation rejects a PENDING job"
    );

    job.markRunning();
    job.markFailed();

    check(
        job.prepareForRetry(),
        "Failed job with retry budget can be prepared"
    );

    bool runningRejected = false;

    try {
        job.prepareForRetry();
    } catch (const std::logic_error&) {
        runningRejected = true;
    }

    check(
        runningRejected,
        "Retry preparation rejects a non-FAILED job"
    );
}

void testJobManagerRetryPolicy() {
    std::cout << "\n--- JobManager Retry Policy Integration ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto createdJob = manager.createJob(
        "REPORT",
        "created-with-policy",
        jobq::JobPriority::High,
        jobq::RetryPolicy{3}
    );

    check(
        createdJob->getPriority() == jobq::JobPriority::High,
        "Created job retains its priority"
    );

    check(
        createdJob->getRetryPolicy().maxRetries == 3,
        "Created job retains its retry policy"
    );

    auto submittedJob = manager.submit(
        "EMAIL",
        "submitted-with-policy",
        jobq::JobPriority::Low,
        jobq::RetryPolicy{2}
    );

    check(
        submittedJob->getPriority() == jobq::JobPriority::Low,
        "Submitted job retains its priority"
    );

    check(
        submittedJob->getRetryPolicy().maxRetries == 2,
        "Submitted job retains its retry policy"
    );

    check(
        manager.getJob(submittedJob->getId()) == submittedJob,
        "JobManager tracks the submitted retryable job"
    );

    check(
        queue.size() == 1,
        "Retry policy does not change normal queue submission"
    );

    check(
        queue.tryPop() == submittedJob,
        "Queue returns the job submitted with retry policy"
    );

    check(
        manager.totalJobs() == 2,
        "Both created and submitted jobs remain registered"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "       Job Retry Policy Tests           \n";
    std::cout << "========================================\n";

    try {
        testDefaultRetryPolicy();
        testRetryLifecycle();
        testRetryPreparationValidation();
        testJobManagerRetryPolicy();
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