#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/ContentionMetrics.h"
#include "metrics/JobMetrics.h"
#include "metrics/MeasuredLock.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

using namespace std::chrono_literals;
using jobq::ContentionMetrics;
using jobq::LockCategory;
using jobq::LockStatistics;

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

void testEmptySnapshot() {
    std::cout << "\n--- Empty Snapshot ---\n";

    ContentionMetrics metrics;
    const auto stats = metrics.snapshot(LockCategory::JobQueue);

    check(stats.acquisitions == 0 && stats.blockedAcquisitions == 0 &&
              stats.overThresholdAcquisitions == 0,
          "A new collector has no acquisitions");
    check(stats.totalWaitNanoseconds == 0 && stats.maxWaitNanoseconds == 0 &&
              stats.holdSamples == 0 && stats.totalHoldNanoseconds == 0 &&
              stats.maxHoldNanoseconds == 0,
          "A new collector has no wait or hold time");
    check(stats.averageWaitNanoseconds() == 0.0 && stats.averageHoldNanoseconds() == 0.0,
          "Averages of an empty snapshot are zero, not NaN");
    check(stats.estimatedWaitPercentileNanoseconds(99) == 0,
          "Percentile estimate of an empty snapshot is zero");
    check(metrics.thresholdNanoseconds() == ContentionMetrics::kDefaultThresholdNanoseconds,
          "Default contention threshold is 1000 ns");
}

void testAggregation() {
    std::cout << "\n--- Acquisition and Hold Aggregation ---\n";

    ContentionMetrics metrics(1000);

    metrics.recordAcquisition(LockCategory::JobQueue, 0, false);
    metrics.recordAcquisition(LockCategory::JobQueue, 100, true);
    metrics.recordAcquisition(LockCategory::JobQueue, 5000, true);

    auto stats = metrics.snapshot(LockCategory::JobQueue);
    check(stats.acquisitions == 3, "Acquisitions are counted");
    check(stats.blockedAcquisitions == 2, "Blocked acquisitions are counted");
    check(stats.totalWaitNanoseconds == 5100, "Total wait is the exact sum");
    check(stats.maxWaitNanoseconds == 5000, "Maximum wait is tracked");
    check(stats.averageWaitNanoseconds() == 1700.0, "Average wait = total / acquisitions");
    check(stats.overThresholdAcquisitions == 1,
          "Only waits >= the threshold count as over threshold");

    metrics.recordHold(LockCategory::JobQueue, 10);
    metrics.recordHold(LockCategory::JobQueue, 20);
    metrics.recordHold(LockCategory::JobQueue, 30);

    stats = metrics.snapshot(LockCategory::JobQueue);
    check(stats.holdSamples == 3 && stats.totalHoldNanoseconds == 60,
          "Hold samples and total hold time aggregate exactly");
    check(stats.maxHoldNanoseconds == 30 && stats.averageHoldNanoseconds() == 20.0,
          "Maximum and average hold time");

    const auto other = metrics.snapshot(LockCategory::JobMetrics);
    check(other.acquisitions == 0 && other.holdSamples == 0,
          "Categories are independent");

    ContentionMetrics lowThreshold(100);
    lowThreshold.recordAcquisition(LockCategory::JobQueue, 99, false);
    lowThreshold.recordAcquisition(LockCategory::JobQueue, 100, false);
    lowThreshold.recordAcquisition(LockCategory::JobQueue, 101, false);
    check(lowThreshold.snapshot(LockCategory::JobQueue).overThresholdAcquisitions == 2,
          "Threshold comparison is >= the configured value");

    ContentionMetrics zeroThreshold(0);
    zeroThreshold.recordAcquisition(LockCategory::JobQueue, 0, false);
    check(zeroThreshold.snapshot(LockCategory::JobQueue).overThresholdAcquisitions == 1,
          "A zero threshold counts every acquisition");

    metrics.reset();
    check(metrics.snapshot(LockCategory::JobQueue).acquisitions == 0,
          "reset() clears every counter");
}

void testBoundaryValues() {
    std::cout << "\n--- Zero, Large and Histogram Values ---\n";

    ContentionMetrics zeros;
    for (int i = 0; i < 10; ++i) {
        zeros.recordAcquisition(LockCategory::JobRegistry, 0, false);
        zeros.recordHold(LockCategory::JobRegistry, 0);
    }
    const auto zeroStats = zeros.snapshot(LockCategory::JobRegistry);
    check(zeroStats.acquisitions == 10 && zeroStats.totalWaitNanoseconds == 0 &&
              zeroStats.maxWaitNanoseconds == 0 && zeroStats.totalHoldNanoseconds == 0,
          "Zero-duration samples are counted and add no time");
    check(zeroStats.waitBuckets[0] == 10 &&
              zeroStats.estimatedWaitPercentileNanoseconds(99) == 0,
          "Zero waits land in bucket 0");

    ContentionMetrics large;
    large.recordAcquisition(
        LockCategory::JobQueue, ContentionMetrics::kMaxSampleNanoseconds, false
    );
    large.recordAcquisition(
        LockCategory::JobQueue, ContentionMetrics::kMaxSampleNanoseconds * 1000, false
    );
    large.recordHold(LockCategory::JobQueue, std::uint64_t{1} << 60);
    const auto largeStats = large.snapshot(LockCategory::JobQueue);
    check(largeStats.maxWaitNanoseconds == ContentionMetrics::kMaxSampleNanoseconds &&
              largeStats.clampedSamples == 2,
          "Samples above the documented limit are clamped and counted");
    check(largeStats.totalWaitNanoseconds == 2 * ContentionMetrics::kMaxSampleNanoseconds,
          "Large values aggregate without wrapping");

    ContentionMetrics buckets;
    for (int i = 0; i < 99; ++i) {
        buckets.recordAcquisition(LockCategory::JobQueue, 10, true);
    }
    buckets.recordAcquisition(LockCategory::JobQueue, 100000, true);
    const auto bucketStats = buckets.snapshot(LockCategory::JobQueue);
    check(bucketStats.estimatedWaitPercentileNanoseconds(50) == 15,
          "p50 estimate is the upper bound of the 8..15 ns bucket");
    check(bucketStats.estimatedWaitPercentileNanoseconds(99) == 15,
          "p99 (rank 99 of 100) is still in the small bucket");
    check(bucketStats.estimatedWaitPercentileNanoseconds(100) == 100000,
          "p100 estimate is capped by the measured maximum");

    ContentionMetrics exact;
    for (int i = 0; i < 100; ++i) {
        exact.recordAcquisition(LockCategory::JobQueue, 1000, false);
    }
    check(exact.snapshot(LockCategory::JobQueue).estimatedWaitPercentileNanoseconds(50) == 1000,
          "Bucket upper bounds never exceed the measured maximum");
}

void testConcurrentRecording() {
    std::cout << "\n--- Concurrent Recording ---\n";

    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;

    ContentionMetrics metrics(50);
    std::vector<std::thread> threads;
    std::atomic<bool> go{false};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kPerThread; ++i) {
                metrics.recordAcquisition(
                    LockCategory::JobQueue,
                    static_cast<std::uint64_t>(i % 100),
                    i % 2 == 0
                );
                metrics.recordHold(LockCategory::JobQueue, 7);
            }
        });
    }

    go = true;
    for (auto& thread : threads) {
        thread.join();
    }

    const auto stats = metrics.snapshot(LockCategory::JobQueue);
    constexpr std::uint64_t total = kThreads * kPerThread;
    constexpr std::uint64_t perThreadWait = (kPerThread / 100) * (99 * 100 / 2);

    check(stats.acquisitions == total && stats.holdSamples == total,
          "No acquisition or hold sample is lost across 8 threads");
    check(stats.blockedAcquisitions == total / 2, "Blocked count is exact");
    check(stats.totalWaitNanoseconds == perThreadWait * kThreads,
          "Total wait equals the deterministic sum");
    check(stats.totalHoldNanoseconds == total * 7 && stats.maxHoldNanoseconds == 7,
          "Hold totals are exact");
    check(stats.maxWaitNanoseconds == 99, "Maximum wait across threads");
    check(stats.overThresholdAcquisitions == kThreads * (kPerThread / 100) * 50,
          "Threshold counting is exact under concurrency (waits 50..99)");

    std::uint64_t bucketTotal = 0;
    for (const auto count : stats.waitBuckets) {
        bucketTotal += count;
    }
    check(bucketTotal == total, "Histogram buckets add up to the acquisition count");
}

void testSnapshotDuringRecording() {
    std::cout << "\n--- Snapshots During Concurrent Recording ---\n";

    ContentionMetrics metrics;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;

    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&] {
            while (!stop.load()) {
                metrics.recordAcquisition(LockCategory::JobMetrics, 3, false);
                metrics.recordHold(LockCategory::JobMetrics, 2);
            }
        });
    }

    bool monotonic = true;
    LockStatistics previous;

    for (int i = 0; i < 2000; ++i) {
        const auto current = metrics.snapshot(LockCategory::JobMetrics);
        monotonic = monotonic && current.acquisitions >= previous.acquisitions &&
                    current.totalWaitNanoseconds >= previous.totalWaitNanoseconds &&
                    current.holdSamples >= previous.holdSamples &&
                    current.maxWaitNanoseconds >= previous.maxWaitNanoseconds;
        previous = current;
    }

    stop = true;
    for (auto& writer : writers) {
        writer.join();
    }

    const auto final = metrics.snapshot(LockCategory::JobMetrics);
    check(monotonic, "Snapshots taken during recording never go backwards");
    check(final.totalWaitNanoseconds == final.acquisitions * 3 &&
              final.totalHoldNanoseconds == final.holdSamples * 2,
          "After the writers stop, totals are consistent with the counts");
}

struct AlwaysBlockedMutex {
    std::mutex inner;
    bool try_lock() { return false; }
    void lock() {
        std::this_thread::sleep_for(5ms);
        inner.lock();
    }
    void unlock() { inner.unlock(); }
};

void testMeasuredLock() {
    std::cout << "\n--- MeasuredLock ---\n";

    ContentionMetrics metrics;
    std::mutex mutex;

    {
        jobq::MeasuredMutexLock lock(mutex, &metrics, LockCategory::JobQueue);
    }

    auto stats = metrics.snapshot(LockCategory::JobQueue);
    check(stats.acquisitions == 1 && stats.blockedAcquisitions == 0 &&
              stats.totalWaitNanoseconds == 0,
          "An uncontended acquisition is counted with zero wait");
    check(stats.holdSamples == 1, "One hold sample per uncontended acquisition");

    {
        jobq::MeasuredMutexLock lock(mutex, nullptr, LockCategory::JobQueue);
    }
    check(metrics.snapshot(LockCategory::JobQueue).acquisitions == 1,
          "With a null collector nothing is recorded");

    AlwaysBlockedMutex blocked;
    {
        jobq::MeasuredLock<AlwaysBlockedMutex, std::unique_lock<AlwaysBlockedMutex>> lock(
            blocked, &metrics, LockCategory::JobRegistry
        );
    }
    stats = metrics.snapshot(LockCategory::JobRegistry);
    check(stats.acquisitions == 1 && stats.blockedAcquisitions == 1,
          "A failed try_lock is recorded as a blocked acquisition");
    check(stats.totalWaitNanoseconds >= 5'000'000,
          "The wait covers the blocking lock() call (at least the 5 ms it slept)");

    {
        jobq::MeasuredMutexLock lock(mutex, &metrics, LockCategory::JobMetrics);
        lock.pauseHold();
        lock.native().unlock();
        std::this_thread::sleep_for(30ms);
        lock.native().lock();
        lock.resumeHold();
    }
    stats = metrics.snapshot(LockCategory::JobMetrics);
    check(stats.acquisitions == 1 && stats.holdSamples == 2,
          "A pause/resume splits the hold into two samples");
    check(stats.maxHoldNanoseconds < 25'000'000,
          "Time spent with the mutex released is not counted as hold time");
}

void testIntegration() {
    std::cout << "\n--- Integration With The Real Engine ---\n";

    constexpr std::uint64_t kJobs = 100;

    {
        jobq::JobQueue queue;
        jobq::JobMetrics metrics;
        jobq::JobManager manager(queue, metrics);
        jobq::JobExecutor executor;
        ContentionMetrics contention;

        executor.registerHandler("T", [](const jobq::Job&) {});

        {
            jobq::WorkerPool pool(queue, executor, 2, metrics);
            pool.start();

            for (std::uint64_t i = 0; i < kJobs; ++i) {
                manager.submit("T", "p");
            }

            pool.stop();
        }

        bool untouched = true;
        for (std::size_t i = 0; i < jobq::kLockCategoryCount; ++i) {
            untouched = untouched &&
                        contention.snapshot(static_cast<LockCategory>(i)).acquisitions == 0;
        }
        check(untouched, "Diagnostics are off by default: an unattached collector records nothing");
        check(metrics.snapshot().completedExecutions == kJobs,
              "The workload itself is unaffected when diagnostics are off");
    }

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;
    ContentionMetrics contention;

    executor.registerHandler("T", [](const jobq::Job&) {});

    queue.setContentionMetrics(&contention);
    manager.setContentionMetrics(&contention);
    metrics.setContentionMetrics(&contention);
    executor.setContentionMetrics(&contention);

    std::vector<std::shared_ptr<jobq::Job>> jobs;

    {
        jobq::WorkerPool pool(queue, executor, 1, metrics);
        pool.start();

        for (std::uint64_t i = 0; i < kJobs; ++i) {
            jobs.push_back(manager.submit("T", "p"));
        }

        pool.stop();
    }

    const auto queueStats = contention.snapshot(LockCategory::JobQueue);
    const auto registryStats = contention.snapshot(LockCategory::JobRegistry);
    const auto metricsStats = contention.snapshot(LockCategory::JobMetrics);
    const auto executorStats = contention.snapshot(LockCategory::JobExecutor);

    check(queueStats.acquisitions == kJobs + kJobs + 1 + 1,
          "Queue lock: 100 pushes + 100 pops + 1 final empty pop + 1 close = 202");
    check(registryStats.acquisitions == kJobs, "Registry lock: one insertion per submission");
    check(metricsStats.acquisitions == 3 * kJobs,
          "Metrics lock: submit + start + complete per job");
    check(executorStats.acquisitions == kJobs, "Executor lock: one handler lookup per job");
    check(registryStats.holdSamples == registryStats.acquisitions &&
              metricsStats.holdSamples == metricsStats.acquisitions &&
              executorStats.holdSamples == executorStats.acquisitions,
          "Locks without condition-variable waits have one hold sample per acquisition");
    check(queueStats.holdSamples >= queueStats.acquisitions,
          "The queue adds hold samples only when a wait splits a hold interval");

    bool allCompleted = true;
    for (const auto& job : jobs) {
        allCompleted = allCompleted && job->getStatus() == jobq::JobStatus::Completed;
    }
    const auto snapshot = metrics.snapshot();
    check(allCompleted && snapshot.completedExecutions == kJobs &&
              snapshot.failedExecutions == 0 && snapshot.submittedJobs == kJobs &&
              snapshot.startedExecutions == kJobs,
          "Enabling diagnostics does not change job states or metrics");

    queue.setContentionMetrics(nullptr);
    manager.setContentionMetrics(nullptr);
    metrics.setContentionMetrics(nullptr);
    executor.setContentionMetrics(nullptr);

    const auto before = contention.snapshot(LockCategory::JobQueue).acquisitions;
    for (int i = 0; i < 10; ++i) {
        (void)queue.size();
    }
    check(contention.snapshot(LockCategory::JobQueue).acquisitions == before,
          "Detaching the collector stops all recording");
}

void testWaitDoesNotCountAsHold() {
    std::cout << "\n--- Condition-Variable Wait Is Not Hold Time ---\n";

    jobq::JobQueue queue;
    ContentionMetrics contention;
    queue.setContentionMetrics(&contention);

    std::shared_ptr<jobq::Job> received;
    std::thread consumer([&] { received = queue.waitAndPop(); });

    std::this_thread::sleep_for(150ms);
    queue.push(std::make_shared<jobq::Job>(1, "T", "p"));
    consumer.join();

    const auto stats = contention.snapshot(LockCategory::JobQueue);
    check(received != nullptr, "The consumer received the job after waiting");
    check(stats.maxHoldNanoseconds < 100'000'000,
          "The 150 ms the consumer slept inside the condition variable is not hold time");
    check(stats.holdSamples > stats.acquisitions - 1,
          "The wait split the consumer's hold into separate samples");

    queue.setContentionMetrics(nullptr);
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobQ Contention Metrics Tests\n";
    std::cout << "========================================\n";

    testEmptySnapshot();
    testAggregation();
    testBoundaryValues();
    testConcurrentRecording();
    testSnapshotDuringRecording();
    testMeasuredLock();
    testIntegration();
    testWaitDoesNotCountAsHold();

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
