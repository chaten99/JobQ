#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

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

void testExponentialDelayCalculation() {
    std::cout << "\n--- Exponential Delay Calculation ---\n";

    jobq::Job job(
        1,
        "EMAIL",
        "backoff-calculation",
        jobq::JobPriority::Normal,
        jobq::RetryPolicy{
            4,
            100ms,
            2.0,
            500ms
        }
    );

    job.markRunning();
    job.markFailed();

    check(
        job.getRetryDelay() == 100ms,
        "First retry uses the initial delay"
    );

    job.prepareForRetry();
    job.markRunning();
    job.markFailed();

    check(
        job.getRetryDelay() == 200ms,
        "Second retry doubles the delay"
    );

    job.prepareForRetry();
    job.markRunning();
    job.markFailed();

    check(
        job.getRetryDelay() == 400ms,
        "Third retry continues exponential backoff"
    );

    job.prepareForRetry();
    job.markRunning();
    job.markFailed();

    check(
        job.getRetryDelay() == 500ms,
        "Backoff delay is capped at maxDelay"
    );
}

void testInvalidRetryPolicy() {
    std::cout << "\n--- Retry Policy Validation ---\n";

    bool negativeDelayRejected = false;

    try {
        jobq::Job job(
            2,
            "EMAIL",
            "negative-delay",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                -100ms,
                2.0,
                1000ms
            }
        );
    } catch (const std::invalid_argument&) {
        negativeDelayRejected = true;
    }

    check(
        negativeDelayRejected,
        "Negative initial delay is rejected"
    );

    bool invalidMultiplierRejected = false;

    try {
        jobq::Job job(
            3,
            "EMAIL",
            "invalid-multiplier",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                100ms,
                0.5,
                1000ms
            }
        );
    } catch (const std::invalid_argument&) {
        invalidMultiplierRejected = true;
    }

    check(
        invalidMultiplierRejected,
        "Multiplier below 1.0 is rejected"
    );
}

void testReadyJobsAreNotBlockedByDelayedRetry() {
    std::cout << "\n--- Delayed Retry Does Not Block Ready Jobs ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);
    jobq::JobExecutor executor;

    std::atomic<int> flakyAttempts{0};

    Clock::time_point firstFailureAt{};
    Clock::time_point secondAttemptAt{};
    Clock::time_point quickJobAt{};

    executor.registerHandler(
        "FLAKY",
        [&](const jobq::Job& job) {
            const int attempt =
                flakyAttempts.fetch_add(1) + 1;

            if (attempt == 1) {
                firstFailureAt = Clock::now();

                throw std::runtime_error(
                    "Temporary failure"
                );
            }

            secondAttemptAt = Clock::now();
        }
    );

    executor.registerHandler(
        "QUICK",
        [&](const jobq::Job&) {
            quickJobAt = Clock::now();
        }
    );

    jobq::WorkerPool pool(queue, executor, 1);

    pool.start();

    auto retryJob = manager.submit(
        "FLAKY",
        "delayed-retry",
        jobq::JobPriority::High,
        jobq::RetryPolicy{
            1,
            300ms,
            2.0,
            1000ms
        }
    );

    auto quickJob = manager.submit(
        "QUICK",
        "must-run-before-retry",
        jobq::JobPriority::Normal,
        jobq::RetryPolicy{0}
    );

    pool.stop();

    const auto retryStatus =
        manager.getStatus(retryJob->getId());

    const auto quickStatus =
        manager.getStatus(quickJob->getId());

    check(
        retryStatus.has_value() &&
            *retryStatus == jobq::JobStatus::Completed,
        "Delayed retry eventually succeeds"
    );

    check(
        quickStatus.has_value() &&
            *quickStatus == jobq::JobStatus::Completed,
        "Ready job completes while retry is waiting"
    );

    check(
        quickJobAt != Clock::time_point{} &&
            secondAttemptAt != Clock::time_point{} &&
            quickJobAt < secondAttemptAt,
        "Ready job executes before the delayed retry"
    );

    const auto actualDelay =
        secondAttemptAt - firstFailureAt;

    check(
        firstFailureAt != Clock::time_point{} &&
            secondAttemptAt != Clock::time_point{} &&
            actualDelay >= 250ms,
        "Retry waits approximately the configured backoff delay"
    );

    check(
        retryJob->getAttemptsMade() == 2,
        "Retry attempt count remains correct"
    );

    check(
        retryJob->getPriority() == jobq::JobPriority::High,
        "Retry preserves original priority"
    );

    check(
        queue.isClosed() && queue.empty(),
        "Shutdown drains delayed retries"
    );
}

void testZeroDelayRetry() {
    std::cout << "\n--- Zero Delay Retry ---\n";

    jobq::JobQueue queue;

    auto job = std::make_shared<jobq::Job>(
        10,
        "EMAIL",
        "zero-delay-retry"
    );

    check(
        queue.pushRetry(job, 0ms),
        "Queue accepts a zero-delay retry"
    );

    check(
        queue.tryPop() == job,
        "Zero-delay retry is immediately ready"
    );

    check(
        queue.empty(),
        "Queue is empty after the retry is popped"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "       Job Retry Backoff Tests          \n";
    std::cout << "========================================\n";

    try {
        testExponentialDelayCalculation();
        testInvalidRetryPolicy();
        testReadyJobsAreNotBlockedByDelayedRetry();
        testZeroDelayRetry();
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