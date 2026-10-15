
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

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
            5,
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

void testRetryPolicyValidation() {
    std::cout << "\n--- Retry Policy Validation ---\n";

    bool negativeInitialDelayRejected = false;

    try {
        jobq::Job job(
            2,
            "EMAIL",
            "negative-initial-delay",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                -100ms,
                2.0,
                1000ms
            }
        );
    } catch (const std::invalid_argument&) {
        negativeInitialDelayRejected = true;
    }

    check(
        negativeInitialDelayRejected,
        "Negative initial delay is rejected"
    );

    bool negativeMaximumDelayRejected = false;

    try {
        jobq::Job job(
            3,
            "EMAIL",
            "negative-maximum-delay",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                100ms,
                2.0,
                -500ms
            }
        );
    } catch (const std::invalid_argument&) {
        negativeMaximumDelayRejected = true;
    }

    check(
        negativeMaximumDelayRejected,
        "Negative maximum delay is rejected"
    );

    bool multiplierBelowOneRejected = false;

    try {
        jobq::Job job(
            4,
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
        multiplierBelowOneRejected = true;
    }

    check(
        multiplierBelowOneRejected,
        "Multiplier below 1.0 is rejected"
    );

    bool nanMultiplierRejected = false;

    try {
        jobq::Job job(
            5,
            "EMAIL",
            "nan-multiplier",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                100ms,
                std::numeric_limits<double>::quiet_NaN(),
                1000ms
            }
        );
    } catch (const std::invalid_argument&) {
        nanMultiplierRejected = true;
    }

    check(
        nanMultiplierRejected,
        "NaN multiplier is rejected"
    );

    bool infiniteMultiplierRejected = false;

    try {
        jobq::Job job(
            6,
            "EMAIL",
            "infinite-multiplier",
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{
                2,
                100ms,
                std::numeric_limits<double>::infinity(),
                1000ms
            }
        );
    } catch (const std::invalid_argument&) {
        infiniteMultiplierRejected = true;
    }

    check(
        infiniteMultiplierRejected,
        "Infinite multiplier is rejected"
    );
}

void testMaximumDelaySmallerThanInitialDelay() {
    std::cout << "\n--- Initial Delay Above Maximum ---\n";

    jobq::Job job(
        7,
        "EMAIL",
        "initial-above-maximum",
        jobq::JobPriority::Normal,
        jobq::RetryPolicy{
            2,
            600ms,
            2.0,
            250ms
        }
    );

    job.markRunning();
    job.markFailed();

    check(
        job.getRetryDelay() == 250ms,
        "Initial retry delay is capped at maxDelay"
    );
}

void testDelayedRetryIsNotImmediatelyReady() {
    std::cout << "\n--- Delayed Retry Readiness ---\n";

    jobq::JobQueue queue;

    auto delayedJob = std::make_shared<jobq::Job>(
        10,
        "EMAIL",
        "delayed-retry",
        jobq::JobPriority::Normal
    );

    const auto scheduledAt = Clock::now();

    const bool accepted = queue.pushRetry(
        delayedJob,
        250ms
    );

    check(
        accepted,
        "Queue accepts a delayed retry"
    );

    check(
        queue.size() == 1,
        "Queue size includes delayed retries"
    );

    check(
        !queue.empty(),
        "Queue is not empty while a retry is delayed"
    );

    check(
        queue.tryPop() == nullptr,
        "tryPop does not return a retry before its deadline"
    );

    const auto poppedJob = queue.waitAndPop();
    const auto elapsed = Clock::now() - scheduledAt;

    check(
        poppedJob == delayedJob,
        "waitAndPop returns the delayed job when ready"
    );

    check(
        elapsed >= 240ms,
        "Delayed job is not returned substantially before its deadline"
    );

    check(
        queue.empty(),
        "Queue becomes empty after delayed retry is retrieved"
    );
}

void testReadyJobsAreNotBlockedByDelayedRetry() {
    std::cout << "\n--- Ready Jobs Are Not Blocked ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);
    jobq::JobExecutor executor;

    std::atomic<int> flakyAttempts{0};

    Clock::time_point firstFailureAt{};
    Clock::time_point secondAttemptAt{};
    Clock::time_point quickJobAt{};

    executor.registerHandler(
        "FLAKY",
        [&](const jobq::Job&) {
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
        "Transiently failing job eventually completes"
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
        "Retry observes the configured backoff delay"
    );

    check(
        retryJob->getAttemptsMade() == 2,
        "Retry attempt count is correct"
    );

    check(
        retryJob->getPriority() == jobq::JobPriority::High,
        "Retry preserves its original priority"
    );

    check(
        queue.isClosed() && queue.empty(),
        "Shutdown drains delayed retries"
    );
}

void testDelayedRetriesRespectPriority() {
    std::cout << "\n--- Priority Among Ready Retries ---\n";

    jobq::JobQueue queue;

    auto lowPriorityJob = std::make_shared<jobq::Job>(
        20,
        "EMAIL",
        "low-priority-retry",
        jobq::JobPriority::Low
    );

    auto highPriorityJob = std::make_shared<jobq::Job>(
        21,
        "EMAIL",
        "high-priority-retry",
        jobq::JobPriority::High
    );

    const bool lowAccepted =
        queue.pushRetry(lowPriorityJob, 80ms);

    const bool highAccepted =
        queue.pushRetry(highPriorityJob, 100ms);

    check(
        lowAccepted && highAccepted,
        "Queue accepts delayed retries with different priorities"
    );

    check(
        queue.size() == 2,
        "Both delayed retries are counted"
    );

    // Wait until both retry deadlines are comfortably past.
    std::this_thread::sleep_for(180ms);

    check(
        queue.tryPop() == highPriorityJob,
        "HIGH priority wins when both retries are ready"
    );

    check(
        queue.tryPop() == lowPriorityJob,
        "LOW priority retry remains queued afterward"
    );

    check(
        queue.empty(),
        "Both ready retries are drained"
    );
}

void testClosedQueueDrainsDelayedRetry() {
    std::cout << "\n--- Shutdown With Delayed Retry ---\n";

    jobq::JobQueue queue;

    queue.close();

    auto retryJob = std::make_shared<jobq::Job>(
        30,
        "EMAIL",
        "retry-after-close",
        jobq::JobPriority::Normal
    );

    const auto scheduledAt = Clock::now();

    check(
        !queue.push(std::make_shared<jobq::Job>(
            31,
            "EMAIL",
            "external-submission-after-close"
        )),
        "Closed queue rejects external submissions"
    );

    check(
        queue.pushRetry(retryJob, 120ms),
        "Internal retry is accepted after queue closure"
    );

    auto poppedJob = queue.waitAndPop();

    const auto elapsed = Clock::now() - scheduledAt;

    check(
        poppedJob == retryJob,
        "Closed queue returns the scheduled retry"
    );

    check(
        elapsed >= 110ms,
        "Closed queue waits for the retry deadline"
    );

    check(
        queue.waitAndPop() == nullptr,
        "Closed queue exits after all work is drained"
    );

    check(
        queue.empty(),
        "Closed queue is empty after draining"
    );
}

void testZeroDelayRetry() {
    std::cout << "\n--- Zero Delay Retry ---\n";

    jobq::JobQueue queue;

    auto job = std::make_shared<jobq::Job>(
        40,
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
    std::cout << "       Job Retry Regression Tests       \n";
    std::cout << "========================================\n";

    try {
        testExponentialDelayCalculation();
        testRetryPolicyValidation();
        testMaximumDelaySmallerThanInitialDelay();
        testDelayedRetryIsNotImmediatelyReady();
        testReadyJobsAreNotBlockedByDelayedRetry();
        testDelayedRetriesRespectPriority();
        testClosedQueueDrainsDelayedRetry();
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
