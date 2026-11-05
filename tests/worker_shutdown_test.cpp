#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
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

// -----------------------------------------------------------------------------
// Test A — Graceful Shutdown With Active Jobs
// -----------------------------------------------------------------------------
void testGracefulShutdownWithActiveJobs() {
    std::cout << "\n--- Test A: Graceful Shutdown With Active Jobs ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobManager manager(queue);

    std::mutex syncMutex;
    std::condition_variable syncCv;
    bool handlerStarted = false;
    bool allowHandlerFinish = false;
    std::atomic<bool> handlerCompleted{false};

    executor.registerHandler(
        "BLOCKING_TASK",
        [&](const jobq::Job&) {
            {
                std::lock_guard<std::mutex> lock(syncMutex);
                handlerStarted = true;
            }
            syncCv.notify_all();

            {
                std::unique_lock<std::mutex> lock(syncMutex);
                syncCv.wait(lock, [&] { return allowHandlerFinish; });
            }

            handlerCompleted.store(true);
        }
    );

    jobq::WorkerPool pool(queue, executor, 1);
    pool.start();

    auto job = manager.submit(
        "BLOCKING_TASK",
        "blocking-payload",
        jobq::JobPriority::Normal
    );

    check(job != nullptr, "Submitted blocking job");

    // Wait until handler is actively executing
    {
        std::unique_lock<std::mutex> lock(syncMutex);
        const bool started = syncCv.wait_for(
            lock,
            5s,
            [&] { return handlerStarted; }
        );
        check(started, "Handler began execution");
    }

    // Initiate shutdown from another thread
    std::atomic<bool> stopCompleted{false};
    std::thread shutdownThread([&] {
        pool.stop();
        stopCompleted.store(true);
    });

    // Verify shutdown does not complete while handler is still blocked
    std::this_thread::sleep_for(150ms);
    check(!stopCompleted.load(), "Shutdown does not return while active handler is running");
    check(!pool.isStopped(), "Worker pool is not marked stopped while active handler is running");
    check(job->getStatus() == jobq::JobStatus::Running, "Job status is Running during shutdown wait");

    // Release the handler to finish
    {
        std::lock_guard<std::mutex> lock(syncMutex);
        allowHandlerFinish = true;
    }
    syncCv.notify_all();

    // Shutdown should now finish cleanly
    shutdownThread.join();

    check(stopCompleted.load(), "Shutdown completed successfully after handler released");
    check(pool.isStopped(), "Worker pool is marked stopped after join");
    check(handlerCompleted.load(), "Handler finished execution logic");
    check(job->getStatus() == jobq::JobStatus::Completed, "Active job reached Completed status");
}

// -----------------------------------------------------------------------------
// Test B — Queued Jobs During Shutdown (Graceful Draining)
// -----------------------------------------------------------------------------
void testQueuedJobsDuringShutdown() {
    std::cout << "\n--- Test B: Queued Jobs During Shutdown (Draining) ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobManager manager(queue);

    std::atomic<int> completedCount{0};

    executor.registerHandler("JOB_FAST", [&](const jobq::Job&) {
        completedCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Submit multiple jobs of varying priorities before/around pool start
    std::vector<std::shared_ptr<jobq::Job>> submittedJobs;
    submittedJobs.reserve(5);

    submittedJobs.push_back(manager.submit("JOB_FAST", "job-1", jobq::JobPriority::High));
    submittedJobs.push_back(manager.submit("JOB_FAST", "job-2", jobq::JobPriority::Normal));
    submittedJobs.push_back(manager.submit("JOB_FAST", "job-3", jobq::JobPriority::Low));
    submittedJobs.push_back(manager.submit("JOB_FAST", "job-4", jobq::JobPriority::Normal));
    submittedJobs.push_back(manager.submit("JOB_FAST", "job-5", jobq::JobPriority::High));

    jobq::WorkerPool pool(queue, executor, 2);
    pool.start();

    // Initiate shutdown immediately while jobs are queued
    pool.stop();

    check(pool.isStopped(), "Worker pool stopped after draining");
    check(completedCount.load() == 5, "All 5 queued jobs were drained to execution");

    bool allCompleted = true;
    for (const auto& job : submittedJobs) {
        if (job->getStatus() != jobq::JobStatus::Completed) {
            allCompleted = false;
        }
    }
    check(allCompleted, "All 5 accepted queued jobs reached Completed status");
    check(queue.empty(), "Job queue is empty after graceful draining");
}

// -----------------------------------------------------------------------------
// Test C — New Submission Rejected During Shutdown
// -----------------------------------------------------------------------------
void testNewSubmissionRejectedDuringShutdown() {
    std::cout << "\n--- Test C: New Submission Rejected During Shutdown ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);

    executor.registerHandler("TASK", [](const jobq::Job&) {});

    jobq::WorkerPool pool(queue, executor, 1, metrics);
    pool.start();

    auto job1 = manager.submit("TASK", "first-job", jobq::JobPriority::Normal);
    check(job1 != nullptr, "First job submitted successfully");

    pool.stop();
    check(pool.isStopped(), "Worker pool stopped");
    check(job1->getStatus() == jobq::JobStatus::Completed, "First job completed");

    // Attempt submission after shutdown
    bool submissionFailedWithException = false;
    std::string failureReason;

    try {
        manager.submit("TASK", "rejected-job", jobq::JobPriority::High);
    } catch (const std::runtime_error& error) {
        submissionFailedWithException = true;
        failureReason = error.what();
    }

    check(submissionFailedWithException, "New submission after shutdown throws std::runtime_error");
    check(
        failureReason.find("queue is closed") != std::string::npos,
        "Error message explains that the queue is closed"
    );

    // Verify registry consistency
    check(manager.totalJobs() == 1, "Job registry still contains only the 1 accepted job");
    check(manager.getJob(job1->getId()) != nullptr, "Original accepted job remains in registry");

    // Verify metrics consistency
    const auto snapshot = metrics.snapshot();
    check(snapshot.submittedJobs == 1, "Metrics do not count rejected submission as submittedJobs");
    check(snapshot.completedExecutions == 1, "Metrics record 1 completed execution");
}

// -----------------------------------------------------------------------------
// Test D — Repeated Shutdown (Idempotency and Concurrency)
// -----------------------------------------------------------------------------
void testRepeatedShutdown() {
    std::cout << "\n--- Test D: Repeated Shutdown (Idempotency & Concurrency) ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    executor.registerHandler("WORK", [](const jobq::Job&) {});

    jobq::WorkerPool pool(queue, executor, 2);
    pool.start();

    // First stop
    pool.stop();
    check(pool.isStopped(), "Pool stopped after first stop() call");

    // Sequential repeated stops
    bool duplicateStopFailed = false;
    try {
        pool.stop();
        pool.stop();
        pool.stop();
    } catch (...) {
        duplicateStopFailed = true;
    }
    check(!duplicateStopFailed, "Sequential duplicate stop() calls do not throw exceptions");
    check(pool.isStopped(), "Pool remains stopped after sequential duplicate calls");

    // Concurrent repeated stops from multiple threads
    std::vector<std::thread> stopThreads;
    std::atomic<bool> concurrentError{false};

    for (int i = 0; i < 6; ++i) {
        stopThreads.emplace_back([&] {
            try {
                pool.stop();
            } catch (...) {
                concurrentError.store(true);
            }
        });
    }

    for (auto& th : stopThreads) {
        th.join();
    }

    check(!concurrentError.load(), "Concurrent stop() calls across 6 threads succeeded safely");
    check(pool.isStopped(), "Pool is stopped after concurrent stop() calls");
}

// -----------------------------------------------------------------------------
// Test E — Shutdown Before Startup
// -----------------------------------------------------------------------------
void testShutdownBeforeStartup() {
    std::cout << "\n--- Test E: Shutdown Before Startup ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;

    {
        jobq::WorkerPool unstartedPool(queue, executor, 2);
        check(!unstartedPool.isStarted(), "Unstarted pool reports isStarted() == false");
        check(!unstartedPool.isStopped(), "Unstarted pool reports isStopped() == false");

        // Invoke stop before start
        unstartedPool.stop();

        check(unstartedPool.isStopped(), "Unstarted pool reports isStopped() == true after stop()");
        check(!unstartedPool.isStarted(), "Unstarted pool still reports isStarted() == false");

        // Attempting to start after stop must fail
        bool startFailedAsExpected = false;
        try {
            unstartedPool.start();
        } catch (const std::logic_error&) {
            startFailedAsExpected = true;
        }

        check(startFailedAsExpected, "Calling start() after stop() on unstarted pool throws std::logic_error");

        // Duplicate stop on unstarted pool is safe
        unstartedPool.stop();
        check(unstartedPool.isStopped(), "Unstarted pool remains stopped after repeated stop()");
    } // unstartedPool destroyed here; verify clean destruction without deadlocks

    check(true, "Unstarted WorkerPool safely destroyed without hanging");
}

// -----------------------------------------------------------------------------
// Test F — Retry During Shutdown
// -----------------------------------------------------------------------------
void testRetryDuringShutdown() {
    std::cout << "\n--- Test F: Retry During Shutdown ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);

    std::atomic<int> attemptCounter{0};

    executor.registerHandler("FLAKY_RETRY", [&](const jobq::Job&) {
        const int attempt = ++attemptCounter;
        if (attempt == 1) {
            throw std::runtime_error("Simulated failure on attempt 1");
        }
        // Attempt 2 succeeds
    });

    jobq::WorkerPool pool(queue, executor, 1, metrics);
    pool.start();

    auto job = manager.submit(
        "FLAKY_RETRY",
        "flaky-payload",
        jobq::JobPriority::High,
        jobq::RetryPolicy{
            .maxRetries = 2,
            .initialDelay = 50ms,
            .multiplier = 1.0,
            .maxDelay = 100ms
        }
    );

    check(job != nullptr, "Submitted flaky retryable job");

    // Initiate shutdown immediately; worker pool must drain the job through its retry
    pool.stop();

    check(pool.isStopped(), "Worker pool stopped after draining retry");
    check(attemptCounter.load() == 2, "Handler was executed exactly 2 times (initial + 1 retry)");
    check(job->getStatus() == jobq::JobStatus::Completed, "Flaky job reached Completed status");
    check(job->getAttemptsMade() == 2, "Job reports exactly 2 attempts made");

    const auto snapshot = metrics.snapshot();
    check(snapshot.submittedJobs == 1, "Metrics: submittedJobs == 1");
    check(snapshot.startedExecutions == 2, "Metrics: startedExecutions == 2");
    check(snapshot.failedExecutions == 1, "Metrics: failedExecutions == 1");
    check(snapshot.completedExecutions == 1, "Metrics: completedExecutions == 1");
    check(snapshot.scheduledRetries == 1, "Metrics: scheduledRetries == 1");
    check(snapshot.finishedExecutions == 2, "Metrics: finishedExecutions == 2");
}

// -----------------------------------------------------------------------------
// Test G — Persistence Compatibility
// -----------------------------------------------------------------------------
void testPersistenceCompatibility() {
    std::cout << "\n--- Test G: Persistence Compatibility ---\n";

    const std::filesystem::path journalPath = "data/shutdown_persistence_test.journal";
    std::filesystem::create_directories(journalPath.parent_path());
    std::filesystem::remove(journalPath);

    {
        jobq::JobQueue queue;
        jobq::JobExecutor executor;
        auto storage = std::make_shared<jobq::FileJobStorage>(journalPath);
        jobq::JobManager manager(queue, storage);

        executor.registerHandler("PERSIST_JOB", [](const jobq::Job&) {});

        auto job1 = manager.submit("PERSIST_JOB", "task-1", jobq::JobPriority::Normal);
        auto job2 = manager.submit("PERSIST_JOB", "task-2", jobq::JobPriority::High);

        check(job1 != nullptr && job2 != nullptr, "Submitted 2 persistent jobs");

        jobq::WorkerPool pool(queue, executor, 2);
        pool.start();

        // Shutdown and drain
        pool.stop();

        check(job1->getStatus() == jobq::JobStatus::Completed, "Persistent job 1 reached Completed");
        check(job2->getStatus() == jobq::JobStatus::Completed, "Persistent job 2 reached Completed");
    }

    // Now reload from journal file in a fresh JobManager instance
    {
        jobq::JobQueue recoveryQueue;
        auto reloadStorage = std::make_shared<jobq::FileJobStorage>(journalPath);
        jobq::JobManager recoveryManager(recoveryQueue, reloadStorage);

        check(recoveryManager.totalJobs() == 2, "Recovery manager restored exactly 2 jobs");

        auto restored1 = recoveryManager.getJob(1);
        auto restored2 = recoveryManager.getJob(2);

        check(restored1 != nullptr, "Restored job 1 found in registry");
        check(restored2 != nullptr, "Restored job 2 found in registry");

        if (restored1 && restored2) {
            check(
                restored1->getStatus() == jobq::JobStatus::Completed,
                "Restored job 1 status is Completed (not overwritten by shutdown)"
            );
            check(
                restored2->getStatus() == jobq::JobStatus::Completed,
                "Restored job 2 status is Completed (not overwritten by shutdown)"
            );
        }

        check(recoveryQueue.empty(), "Recovery queue has 0 pending items (completed jobs not re-enqueued)");
    }

    std::filesystem::remove(journalPath);
    check(!std::filesystem::exists(journalPath), "Cleaned up temporary test journal file");
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "      JobQ Worker Shutdown Tests\n";
    std::cout << "========================================\n";

    testGracefulShutdownWithActiveJobs();
    testQueuedJobsDuringShutdown();
    testNewSubmissionRejectedDuringShutdown();
    testRepeatedShutdown();
    testShutdownBeforeStartup();
    testRetryDuringShutdown();
    testPersistenceCompatibility();

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    if (testsFailed == 0) {
        std::cout << "Result: ALL TESTS PASSED\n";
    } else {
        std::cout << "Result: SOME TESTS FAILED\n";
    }
    std::cout << "========================================\n";

    return (testsFailed == 0) ? 0 : 1;
}
