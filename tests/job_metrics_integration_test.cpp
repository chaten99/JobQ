#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

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

void verifySnapshotInvariants(
    const jobq::JobMetricsSnapshot& snapshot,
    std::uint64_t expectedSubmitted,
    std::uint64_t expectedStarted,
    std::uint64_t expectedCompleted,
    std::uint64_t expectedFailed,
    std::uint64_t expectedRetries
) {
    check(
        snapshot.submittedJobs == expectedSubmitted,
        "Snapshot matches expected submittedJobs: " +
            std::to_string(expectedSubmitted)
    );
    check(
        snapshot.startedExecutions == expectedStarted,
        "Snapshot matches expected startedExecutions: " +
            std::to_string(expectedStarted)
    );
    check(
        snapshot.completedExecutions == expectedCompleted,
        "Snapshot matches expected completedExecutions: " +
            std::to_string(expectedCompleted)
    );
    check(
        snapshot.failedExecutions == expectedFailed,
        "Snapshot matches expected failedExecutions: " +
            std::to_string(expectedFailed)
    );
    check(
        snapshot.scheduledRetries == expectedRetries,
        "Snapshot matches expected scheduledRetries: " +
            std::to_string(expectedRetries)
    );

    check(
        snapshot.finishedExecutions ==
            snapshot.completedExecutions + snapshot.failedExecutions,
        "Invariant: finishedExecutions == completedExecutions + failedExecutions"
    );

    check(
        snapshot.startedExecutions == snapshot.finishedExecutions,
        "Invariant: startedExecutions == finishedExecutions when workload drained"
    );

    check(
        snapshot.scheduledRetries <= snapshot.failedExecutions,
        "Invariant: scheduledRetries <= failedExecutions"
    );

    check(
        snapshot.totalExecutionTime >= 0ns,
        "Invariant: totalExecutionTime is non-negative"
    );

    check(
        snapshot.elapsedTime >= 0ns,
        "Invariant: elapsedTime is non-negative"
    );

    check(
        std::isfinite(snapshot.throughputJobsPerSecond) &&
            snapshot.throughputJobsPerSecond >= 0.0,
        "Invariant: throughput is finite and non-negative"
    );

    if (snapshot.finishedExecutions > 0) {
        const auto expectedAverageNs =
            snapshot.totalExecutionTime.count() /
            static_cast<std::chrono::nanoseconds::rep>(
                snapshot.finishedExecutions
            );

        check(
            snapshot.averageExecutionTime.count() == expectedAverageNs,
            "Invariant: averageExecutionTime equals total / finished"
        );
    } else {
        check(
            snapshot.averageExecutionTime == 0ns,
            "Invariant: averageExecutionTime is 0ns when 0 finished"
        );
        check(
            snapshot.throughputJobsPerSecond == 0.0,
            "Invariant: throughput is 0.0 when 0 finished"
        );
    }
}

// ============================================================================
// Test Case A — Multiple Workers Executing Successful Jobs with Concurrency Proof
// ============================================================================
void testSuccessfulJobsWithProvenConcurrency() {
    std::cout << "\n--- Test Case A: Multiple Workers with Proven Concurrency ---\n";

    constexpr std::size_t workerCount = 4;
    constexpr std::size_t totalJobs = 100;
    constexpr std::size_t syncJobCount = 4;

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;

    std::atomic<int> activeHandlers{0};
    std::atomic<int> maxSimultaneousHandlers{0};
    std::latch syncLatch(static_cast<std::ptrdiff_t>(syncJobCount));
    std::atomic<std::size_t> executedJobsCount{0};

    executor.registerHandler(
        "SYNC_TASK",
        [&](const jobq::Job&) {
            const int currentActive = ++activeHandlers;

            int currentMax = maxSimultaneousHandlers.load(
                std::memory_order_relaxed
            );
            while (currentActive > currentMax &&
                   !maxSimultaneousHandlers.compare_exchange_weak(
                       currentMax,
                       currentActive,
                       std::memory_order_relaxed
                   )) {}

            syncLatch.arrive_and_wait();

            --activeHandlers;
            ++executedJobsCount;
        }
    );

    executor.registerHandler(
        "WORK_TASK",
        [&](const jobq::Job&) {
            const int currentActive = ++activeHandlers;

            int currentMax = maxSimultaneousHandlers.load(
                std::memory_order_relaxed
            );
            while (currentActive > currentMax &&
                   !maxSimultaneousHandlers.compare_exchange_weak(
                       currentMax,
                       currentActive,
                       std::memory_order_relaxed
                   )) {}

            std::this_thread::yield();

            --activeHandlers;
            ++executedJobsCount;
        }
    );

    jobq::WorkerPool pool(queue, executor, workerCount, metrics);
    pool.start();

    std::vector<std::shared_ptr<jobq::Job>> submittedJobs;
    submittedJobs.reserve(totalJobs);

    for (std::size_t i = 0; i < syncJobCount; ++i) {
        auto job = manager.submit(
            "SYNC_TASK",
            "sync-" + std::to_string(i),
            jobq::JobPriority::High
        );
        submittedJobs.push_back(std::move(job));
    }

    for (std::size_t i = syncJobCount; i < totalJobs; ++i) {
        auto job = manager.submit(
            "WORK_TASK",
            "work-" + std::to_string(i),
            jobq::JobPriority::Normal
        );
        submittedJobs.push_back(std::move(job));
    }

    pool.stop();

    check(
        executedJobsCount.load() == totalJobs,
        "All 100 jobs were executed by the worker pool"
    );

    check(
        maxSimultaneousHandlers.load() == static_cast<int>(syncJobCount),
        "Proven concurrency: exactly 4 workers entered handlers simultaneously"
    );

    check(
        maxSimultaneousHandlers.load() > 1,
        "Maximum simultaneous handlers strictly exceeds 1"
    );

    bool allCompleted = true;
    for (const auto& job : submittedJobs) {
        const auto status = manager.getStatus(job->getId());
        if (!status.has_value() || *status != jobq::JobStatus::Completed) {
            allCompleted = false;
            break;
        }
    }
    check(allCompleted, "All 100 submitted jobs reached COMPLETED status");

    const auto snapshot = metrics.snapshot();
    verifySnapshotInvariants(
        snapshot,
        /*expectedSubmitted=*/100,
        /*expectedStarted=*/100,
        /*expectedCompleted=*/100,
        /*expectedFailed=*/0,
        /*expectedRetries=*/0
    );
}

// ============================================================================
// Test Case B — Mixed Successful and Failed Executions
// ============================================================================
void testMixedSuccessfulAndFailedExecutions() {
    std::cout << "\n--- Test Case B: Mixed Successful and Failed Executions ---\n";

    constexpr std::size_t workerCount = 4;
    constexpr std::size_t successJobsCount = 30;
    constexpr std::size_t failNoRetryCount = 20;
    constexpr std::size_t failWithRetriesCount = 10;
    constexpr std::size_t retriesPerFailingJob = 2; // 3 total attempts per job

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;

    executor.registerHandler(
        "SUCCESS_OP",
        [](const jobq::Job&) {
            // Succeeded cleanly
        }
    );

    executor.registerHandler(
        "FAIL_PERM",
        [](const jobq::Job&) {
            throw std::runtime_error("Intentional permanent failure");
        }
    );

    executor.registerHandler(
        "FAIL_RETRYABLE",
        [](const jobq::Job&) {
            throw std::runtime_error("Intentional retryable failure");
        }
    );

    jobq::WorkerPool pool(queue, executor, workerCount, metrics);
    pool.start();

    std::vector<std::shared_ptr<jobq::Job>> successJobs;
    successJobs.reserve(successJobsCount);
    for (std::size_t i = 0; i < successJobsCount; ++i) {
        auto job = manager.submit(
            "SUCCESS_OP",
            "success-" + std::to_string(i),
            jobq::JobPriority::Normal
        );
        successJobs.push_back(std::move(job));
    }

    std::vector<std::shared_ptr<jobq::Job>> failNoRetryJobs;
    failNoRetryJobs.reserve(failNoRetryCount);
    for (std::size_t i = 0; i < failNoRetryCount; ++i) {
        jobq::RetryPolicy noRetries;
        noRetries.maxRetries = 0;
        auto job = manager.submit(
            "FAIL_PERM",
            "fail-no-retry-" + std::to_string(i),
            jobq::JobPriority::Normal,
            noRetries
        );
        failNoRetryJobs.push_back(std::move(job));
    }

    std::vector<std::shared_ptr<jobq::Job>> failWithRetryJobs;
    failWithRetryJobs.reserve(failWithRetriesCount);
    for (std::size_t i = 0; i < failWithRetriesCount; ++i) {
        jobq::RetryPolicy retryPolicy;
        retryPolicy.maxRetries = retriesPerFailingJob;
        retryPolicy.initialDelay = 1ms;
        retryPolicy.multiplier = 1.0;
        retryPolicy.maxDelay = 5ms;

        auto job = manager.submit(
            "FAIL_RETRYABLE",
            "fail-retry-" + std::to_string(i),
            jobq::JobPriority::Normal,
            retryPolicy
        );
        failWithRetryJobs.push_back(std::move(job));
    }

    pool.stop();

    // Verify final statuses
    bool allSuccessPassed = true;
    for (const auto& job : successJobs) {
        const auto status = manager.getStatus(job->getId());
        if (!status.has_value() || *status != jobq::JobStatus::Completed) {
            allSuccessPassed = false;
        }
    }
    check(allSuccessPassed, "All success jobs reached COMPLETED status");

    bool allPermanentFailuresPassed = true;
    for (const auto& job : failNoRetryJobs) {
        const auto status = manager.getStatus(job->getId());
        if (!status.has_value() || *status != jobq::JobStatus::Failed) {
            allPermanentFailuresPassed = false;
        }
    }
    check(allPermanentFailuresPassed, "All fail-no-retry jobs reached FAILED status");

    bool allRetryFailuresPassed = true;
    for (const auto& job : failWithRetryJobs) {
        const auto status = manager.getStatus(job->getId());
        if (!status.has_value() ||
            *status != jobq::JobStatus::Failed ||
            job->getAttemptsMade() != retriesPerFailingJob + 1) {
            allRetryFailuresPassed = false;
        }
    }
    check(
        allRetryFailuresPassed,
        "All fail-with-retry jobs exhausted their retries and reached FAILED status"
    );

    constexpr std::uint64_t expectedSubmitted =
        successJobsCount + failNoRetryCount + failWithRetriesCount; // 60
    constexpr std::uint64_t expectedCompleted =
        successJobsCount; // 30
    constexpr std::uint64_t expectedFailed =
        failNoRetryCount + (failWithRetriesCount * (retriesPerFailingJob + 1)); // 20 + 30 = 50
    constexpr std::uint64_t expectedStarted =
        expectedCompleted + expectedFailed; // 80
    constexpr std::uint64_t expectedRetries =
        failWithRetriesCount * retriesPerFailingJob; // 20

    const auto snapshot = metrics.snapshot();
    verifySnapshotInvariants(
        snapshot,
        expectedSubmitted,
        expectedStarted,
        expectedCompleted,
        expectedFailed,
        expectedRetries
    );
}

// ============================================================================
// Test Case C — Retry Metrics Integration
// ============================================================================
void testRetryMetricsIntegration() {
    std::cout << "\n--- Test Case C: Retry Metrics Integration ---\n";

    // Scenario C1: Job fails on attempt 1, retry scheduled, succeeds on attempt 2
    {
        jobq::JobQueue queue;
        jobq::JobMetrics metrics;
        jobq::JobManager manager(queue, metrics);
        jobq::JobExecutor executor;

        std::atomic<int> attemptCounter{0};

        executor.registerHandler(
            "FLAKY_TASK",
            [&](const jobq::Job&) {
                const int attempt = ++attemptCounter;
                if (attempt == 1) {
                    throw std::runtime_error("Attempt 1 fails transiently");
                }
            }
        );

        jobq::WorkerPool pool(queue, executor, 2, metrics);
        pool.start();

        jobq::RetryPolicy policy;
        policy.maxRetries = 1;
        policy.initialDelay = 2ms;

        auto job = manager.submit(
            "FLAKY_TASK",
            "flaky-payload",
            jobq::JobPriority::Normal,
            policy
        );

        pool.stop();

        const auto status = manager.getStatus(job->getId());
        check(
            status.has_value() && *status == jobq::JobStatus::Completed,
            "Transiently failing job completed after retry"
        );
        check(
            job->getAttemptsMade() == 2,
            "Job records exactly 2 attempts made"
        );

        const auto snapshot = metrics.snapshot();
        verifySnapshotInvariants(
            snapshot,
            /*expectedSubmitted=*/1,
            /*expectedStarted=*/2,
            /*expectedCompleted=*/1,
            /*expectedFailed=*/1,
            /*expectedRetries=*/1
        );
    }

    // Scenario C2: Job exhausts multiple retries and stays FAILED
    {
        jobq::JobQueue queue;
        jobq::JobMetrics metrics;
        jobq::JobManager manager(queue, metrics);
        jobq::JobExecutor executor;

        executor.registerHandler(
            "ALWAYS_FAIL",
            [](const jobq::Job&) {
                throw std::runtime_error("Always fail");
            }
        );

        jobq::WorkerPool pool(queue, executor, 2, metrics);
        pool.start();

        jobq::RetryPolicy policy;
        policy.maxRetries = 2; // 3 attempts total, 2 scheduled retries
        policy.initialDelay = 1ms;

        auto job = manager.submit(
            "ALWAYS_FAIL",
            "exhaust-payload",
            jobq::JobPriority::High,
            policy
        );

        pool.stop();

        const auto status = manager.getStatus(job->getId());
        check(
            status.has_value() && *status == jobq::JobStatus::Failed,
            "Exhausted job final status is FAILED"
        );
        check(
            job->getAttemptsMade() == 3,
            "Job made 3 attempts before exhausting retries"
        );

        const auto snapshot = metrics.snapshot();
        verifySnapshotInvariants(
            snapshot,
            /*expectedSubmitted=*/1,
            /*expectedStarted=*/3,
            /*expectedCompleted=*/0,
            /*expectedFailed=*/3,
            /*expectedRetries=*/2
        );
    }
}

// ============================================================================
// Test Case D — Concurrent Metrics Updates (Stress Testing)
// ============================================================================
void testConcurrentMetricsUpdates() {
    std::cout << "\n--- Test Case D: Concurrent Submissions and Worker Processing ---\n";

    constexpr std::size_t workerCount = 4;
    constexpr std::size_t producerCount = 4;
    constexpr std::size_t jobsPerProducer = 50;
    constexpr std::size_t totalJobs = producerCount * jobsPerProducer; // 200 jobs

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;

    std::atomic<std::size_t> completedOps{0};
    std::atomic<std::size_t> failedOps{0};

    executor.registerHandler(
        "CONC_FAST",
        [&](const jobq::Job&) {
            ++completedOps;
        }
    );

    executor.registerHandler(
        "CONC_NORMAL",
        [&](const jobq::Job&) {
            std::this_thread::yield();
            ++completedOps;
        }
    );

    executor.registerHandler(
        "CONC_FAIL",
        [&](const jobq::Job&) {
            ++failedOps;
            throw std::runtime_error("Concurrent failure");
        }
    );

    executor.registerHandler(
        "CONC_RETRY",
        [&](const jobq::Job& job) {
            if (job.getAttemptsMade() == 1) {
                ++failedOps;
                throw std::runtime_error("Attempt 1 fails");
            }
            ++completedOps;
        }
    );

    jobq::WorkerPool pool(queue, executor, workerCount, metrics);
    pool.start();

    // 4 concurrent producers submitting jobs simultaneously
    // Breakdown per producer (50 jobs each):
    // - 20 CONC_FAST (maxRetries = 0) -> 20 starts, 20 completes
    // - 15 CONC_NORMAL (maxRetries = 0) -> 15 starts, 15 completes
    // - 10 CONC_FAIL (maxRetries = 0) -> 10 starts, 10 fails
    // -  5 CONC_RETRY (maxRetries = 1, 1ms) -> 5 * 2 = 10 starts, 5 fails, 5 retries, 5 completes
    // Per producer:
    //   Submitted: 50
    //   Starts: 20 + 15 + 10 + 10 = 55
    //   Completed: 20 + 15 + 5 = 40
    //   Failed: 10 + 5 = 15
    //   Retries: 5
    // Across 4 producers:
    //   Submitted: 200
    //   Starts: 220
    //   Completed: 160
    //   Failed: 60
    //   Retries: 20
    //   Finished: 220

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (std::size_t p = 0; p < producerCount; ++p) {
        producers.emplace_back([&manager, p]() {
            for (std::size_t i = 0; i < 20; ++i) {
                manager.submit(
                    "CONC_FAST",
                    "p" + std::to_string(p) + "-fast-" + std::to_string(i),
                    jobq::JobPriority::Normal
                );
            }
            for (std::size_t i = 0; i < 15; ++i) {
                manager.submit(
                    "CONC_NORMAL",
                    "p" + std::to_string(p) + "-norm-" + std::to_string(i),
                    jobq::JobPriority::Normal
                );
            }
            for (std::size_t i = 0; i < 10; ++i) {
                jobq::RetryPolicy noRetries;
                noRetries.maxRetries = 0;
                manager.submit(
                    "CONC_FAIL",
                    "p" + std::to_string(p) + "-fail-" + std::to_string(i),
                    jobq::JobPriority::Low,
                    noRetries
                );
            }
            for (std::size_t i = 0; i < 5; ++i) {
                jobq::RetryPolicy retryPolicy;
                retryPolicy.maxRetries = 1;
                retryPolicy.initialDelay = 1ms;
                manager.submit(
                    "CONC_RETRY",
                    "p" + std::to_string(p) + "-retry-" + std::to_string(i),
                    jobq::JobPriority::High,
                    retryPolicy
                );
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    pool.stop();

    constexpr std::uint64_t expectedSubmitted = 200;
    constexpr std::uint64_t expectedCompleted = 160;
    constexpr std::uint64_t expectedFailed = 60;
    constexpr std::uint64_t expectedStarted = 220;
    constexpr std::uint64_t expectedRetries = 20;

    check(
        completedOps.load() == expectedCompleted,
        "Handler completions count matches expected: " +
            std::to_string(expectedCompleted)
    );
    check(
        failedOps.load() == expectedFailed,
        "Handler failures count matches expected: " +
            std::to_string(expectedFailed)
    );

    const auto snapshot = metrics.snapshot();
    verifySnapshotInvariants(
        snapshot,
        expectedSubmitted,
        expectedStarted,
        expectedCompleted,
        expectedFailed,
        expectedRetries
    );
}

// ============================================================================
// Test Case E — Metrics Snapshot Consistency & Edge Cases
// ============================================================================
void testSnapshotConsistencyAndEdgeCases() {
    std::cout << "\n--- Test Case E: Snapshot Invariants and Zero-Execution Safety ---\n";

    // 1. Snapshot on empty runtime (0 jobs submitted, 0 workers executed)
    {
        jobq::JobQueue queue;
        jobq::JobMetrics metrics;
        jobq::JobManager manager(queue, metrics);
        jobq::JobExecutor executor;

        jobq::WorkerPool pool(queue, executor, 2, metrics);
        pool.start();
        pool.stop();

        const auto snapshot = metrics.snapshot();
        verifySnapshotInvariants(
            snapshot,
            /*expectedSubmitted=*/0,
            /*expectedStarted=*/0,
            /*expectedCompleted=*/0,
            /*expectedFailed=*/0,
            /*expectedRetries=*/0
        );

        check(
            snapshot.averageExecutionTime == 0ns,
            "Empty snapshot has 0ns averageExecutionTime without division by zero"
        );
        check(
            snapshot.throughputJobsPerSecond == 0.0,
            "Empty snapshot has 0.0 throughput without division by zero"
        );
    }

    // 2. Thread-safe snapshot read during active concurrent workload
    {
        jobq::JobQueue queue;
        jobq::JobMetrics metrics;
        jobq::JobManager manager(queue, metrics);
        jobq::JobExecutor executor;

        executor.registerHandler(
            "SPIN_TASK",
            [](const jobq::Job&) {
                std::this_thread::yield();
            }
        );

        jobq::WorkerPool pool(queue, executor, 4, metrics);
        pool.start();

        for (int i = 0; i < 50; ++i) {
            manager.submit("SPIN_TASK", std::to_string(i));
        }

        std::atomic<bool> readerRunning{true};
        std::atomic<std::size_t> successfulSnapshotReads{0};

        std::thread snapshotReader([&]() {
            while (readerRunning.load(std::memory_order_relaxed)) {
                const auto snap = metrics.snapshot();
                if (snap.finishedExecutions ==
                    snap.completedExecutions + snap.failedExecutions) {
                    ++successfulSnapshotReads;
                }
                std::this_thread::yield();
            }
        });

        for (int i = 50; i < 100; ++i) {
            manager.submit("SPIN_TASK", std::to_string(i));
        }

        pool.stop();

        readerRunning.store(false, std::memory_order_relaxed);
        snapshotReader.join();

        check(
            successfulSnapshotReads.load() > 0,
            "Snapshots were read concurrently while workers processed jobs"
        );

        const auto finalSnapshot = metrics.snapshot();
        verifySnapshotInvariants(
            finalSnapshot,
            /*expectedSubmitted=*/100,
            /*expectedStarted=*/100,
            /*expectedCompleted=*/100,
            /*expectedFailed=*/0,
            /*expectedRetries=*/0
        );
    }
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " JobQ Metrics Integration & Concurrency\n";
    std::cout << "========================================\n";

    try {
        testSuccessfulJobsWithProvenConcurrency();
        testMixedSuccessfulAndFailedExecutions();
        testRetryMetricsIntegration();
        testConcurrentMetricsUpdates();
        testSnapshotConsistencyAndEdgeCases();
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
