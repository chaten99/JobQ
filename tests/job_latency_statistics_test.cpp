#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/ExecutionObserver.h"
#include "metrics/JobMetrics.h"
#include "metrics/LatencyStatistics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

using namespace std::chrono_literals;
using jobq::LatencySamples;
using ns = std::chrono::nanoseconds;

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

LatencySamples samplesOf(const std::vector<std::int64_t>& values) {
    LatencySamples samples;
    for (const auto value : values) {
        samples.add(ns(value));
    }
    return samples;
}

void testEmpty() {
    std::cout << "\n--- Empty Sample Collection ---\n";

    LatencySamples samples;

    check(samples.empty() && samples.count() == 0, "Empty collection reports no samples");
    check(!samples.summarize().has_value(), "summarize() of an empty set is nullopt, not zeros");
    check(!samples.percentile(50).has_value(), "percentile() of an empty set is nullopt");
    check(samples.total() == ns(0), "Total of an empty set is zero");

    bool threw = false;
    try {
        LatencySamples::nearestRankIndex(0, 50);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "nearestRankIndex(0, p) is rejected");
}

void testSingleSample() {
    std::cout << "\n--- Single Sample ---\n";

    const auto summary = samplesOf({42}).summarize();

    check(summary.has_value() && summary->count == 1, "One sample is counted");
    check(summary->minimum == ns(42) && summary->maximum == ns(42),
          "Min and max equal the sample");
    check(summary->averageNanoseconds == 42.0, "Average equals the sample");
    check(summary->p50 == ns(42) && summary->p95 == ns(42) && summary->p99 == ns(42),
          "Every percentile equals the sample");
}

void testOddAndEven() {
    std::cout << "\n--- Odd and Even Counts ---\n";

    const auto odd = samplesOf({9, 1, 5, 3, 7, 2, 8, 4, 6}).summarize();
    check(odd->count == 9 && odd->minimum == ns(1) && odd->maximum == ns(9),
          "Odd set: count, min, max (input order does not matter)");
    check(odd->averageNanoseconds == 5.0, "Odd set: average = 45 / 9");
    check(odd->p50 == ns(5), "Odd set: p50 = ceil(0.50*9) = rank 5 -> 5");
    check(odd->p95 == ns(9) && odd->p99 == ns(9),
          "Odd set: p95 and p99 resolve to rank 9");

    const auto even = samplesOf({4, 2, 3, 1}).summarize();
    check(even->p50 == ns(2),
          "Even set: nearest-rank p50 is the lower middle sample (rank 2)");
    check(even->averageNanoseconds == 2.5, "Even set: average = 10 / 4");
    check(even->p95 == ns(4) && even->p99 == ns(4), "Even set: upper percentiles = max");
}

void testPercentileConvention() {
    std::cout << "\n--- Nearest-Rank Convention ---\n";

    std::vector<std::int64_t> hundred;
    for (std::int64_t i = 1; i <= 100; ++i) {
        hundred.push_back(i);
    }
    const auto s100 = samplesOf(hundred).summarize();
    check(s100->p50 == ns(50) && s100->p95 == ns(95) && s100->p99 == ns(99),
          "1..100: p50=50, p95=95, p99=99");

    std::vector<std::int64_t> thousand;
    for (std::int64_t i = 1; i <= 1000; ++i) {
        thousand.push_back(i);
    }
    const auto s1000 = samplesOf(thousand).summarize();
    check(s1000->p50 == ns(500) && s1000->p95 == ns(950) && s1000->p99 == ns(990),
          "1..1000: p50=500, p95=950, p99=990");

    check(LatencySamples::nearestRankIndex(1, 0) == 0 &&
              LatencySamples::nearestRankIndex(1, 100) == 0,
          "One sample: every percentile maps to index 0");
    check(LatencySamples::nearestRankIndex(10, 0) == 0,
          "p0 maps to the minimum");
    check(LatencySamples::nearestRankIndex(10, 100) == 9,
          "p100 maps to the maximum");
    check(LatencySamples::nearestRankIndex(1000, 99) == 989,
          "n=1000, p99 -> rank 990 (index 989)");
    check(LatencySamples::nearestRankIndex(101, 99) == 99,
          "n=101, p99 -> ceil(99.99) = rank 100 (index 99)");

    bool threw = false;
    try {
        samplesOf({1}).percentile(101);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "Percentiles above 100 are rejected");

    check(samplesOf({10, 20, 30}).percentile(50) == ns(20),
          "percentile(50) matches the summary convention");
}

void testBoundaryValues() {
    std::cout << "\n--- Zero, Tiny and Large Durations ---\n";

    const auto zeros = samplesOf({0, 0, 0}).summarize();
    check(zeros->minimum == ns(0) && zeros->maximum == ns(0) &&
              zeros->averageNanoseconds == 0.0 && zeros->p99 == ns(0),
          "Zero-duration samples are valid and summarize to zero");

    const auto tiny = samplesOf({1, 2}).summarize();
    check(tiny->minimum == ns(1) && tiny->averageNanoseconds == 1.5,
          "Nanosecond-scale samples keep fractional averages");

    LatencySamples large;
    for (int i = 0; i < 3; ++i) {
        large.add(ns(4'000'000'000'000'000'000LL));
    }
    const auto largeSummary = large.summarize();
    check(!large.total().has_value(),
          "Total beyond int64 range is reported as unavailable, not wrapped");
    check(largeSummary->averageNanoseconds == 4.0e18 &&
              largeSummary->maximum == ns(4'000'000'000'000'000'000LL),
          "Average stays exact when the sum exceeds int64");

    LatencySamples huge;
    for (int i = 0; i < 4; ++i) {
        huge.add(ns(9'000'000'000'000'000'000LL));
    }
    check(huge.summarize()->averageNanoseconds == 9.0e18,
          "Average stays exact when the sum exceeds uint64");

    LatencySamples negative;
    negative.add(ns(5));
    bool threw = false;
    try {
        negative.add(ns(-1));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw && negative.count() == 1 && negative.total() == ns(5),
          "Negative durations are rejected and leave the collection unchanged");
}

void testAggregation() {
    std::cout << "\n--- Average, Min/Max, Merge ---\n";

    const auto samples = samplesOf({10, 20, 30, 40});
    check(samples.total() == ns(100), "Total is the exact sum");
    check(samples.summarize()->averageNanoseconds == 25.0, "Average = sum / count");

    LatencySamples left = samplesOf({1, 2, 3});
    const LatencySamples right = samplesOf({10, 20});
    left.merge(right);
    check(left.count() == 5 && left.total() == ns(36),
          "Merge keeps every sample and the exact sum");
    check(left.summarize()->maximum == ns(20) && left.summarize()->minimum == ns(1),
          "Merged min and max are correct");
    check(right.count() == 2, "Merge does not modify its source");

    LatencySamples carryLeft;
    LatencySamples carryRight;
    for (int i = 0; i < 3; ++i) {
        carryLeft.add(ns(4'000'000'000'000'000'000LL));
        carryRight.add(ns(4'000'000'000'000'000'000LL));
    }
    carryLeft.merge(carryRight);
    check(carryLeft.summarize()->averageNanoseconds == 4.0e18,
          "Merging sums that overflow uint64 keeps the average exact");
}

void testConcurrentCollection() {
    std::cout << "\n--- Concurrent Collection (per-thread collectors) ---\n";

    constexpr int kThreads = 8;
    constexpr int kPerThread = 10000;

    std::vector<LatencySamples> perThread(kThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                perThread[t].add(ns(t * kPerThread + i));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    LatencySamples merged;
    for (const auto& samples : perThread) {
        merged.merge(samples);
    }

    constexpr std::int64_t count = kThreads * kPerThread;
    const auto summary = merged.summarize();

    check(summary->count == static_cast<std::size_t>(count),
          "No samples dropped or duplicated across 8 threads");
    check(merged.total() == ns(count * (count - 1) / 2),
          "Exact sum of 0..79999");
    check(summary->minimum == ns(0) && summary->maximum == ns(count - 1),
          "Min and max span every thread's samples");
    check(summary->p50 == ns(count / 2 - 1),
          "Median of the merged set is deterministic");
}

void testQueueReadyTimestamps() {
    std::cout << "\n--- Queue-Ready Timestamps ---\n";

    using Clock = std::chrono::steady_clock;

    jobq::JobQueue queue;

    auto immediate = std::make_shared<jobq::Job>(1, "T", "p");
    check(immediate->queueReadyTime() == Clock::time_point{},
          "A job that was never queued has no ready time");

    const auto before = Clock::now();
    queue.push(immediate);
    const auto after = Clock::now();
    check(immediate->queueReadyTime() >= before && immediate->queueReadyTime() <= after,
          "push() stamps the moment the job enters the ready queue");

    auto zeroDelay = std::make_shared<jobq::Job>(2, "T", "p");
    const auto beforeZero = Clock::now();
    queue.pushRetry(zeroDelay, 0ms);
    const auto afterZero = Clock::now();
    check(zeroDelay->queueReadyTime() >= beforeZero &&
              zeroDelay->queueReadyTime() <= afterZero,
          "Zero-delay retry is stamped when it becomes ready");

    auto delayed = std::make_shared<jobq::Job>(3, "T", "p");
    const auto beforeDelayed = Clock::now();
    queue.pushRetry(delayed, 30ms);
    check(delayed->queueReadyTime() == Clock::time_point{},
          "A delayed retry has no ready time while it waits out its backoff");

    queue.tryPop();
    queue.tryPop();
    const auto popped = queue.waitAndPop();
    const auto afterPromotion = Clock::now();

    check(popped && popped->getId() == 3, "Delayed retry is delivered after its delay");
    check(popped->queueReadyTime() >= beforeDelayed + 30ms &&
              popped->queueReadyTime() <= afterPromotion,
          "Promotion stamps the eligibility time (backoff excluded from queue wait)");

    auto restamped = std::make_shared<jobq::Job>(4, "T", "p");
    queue.push(restamped);
    const auto first = restamped->queueReadyTime();
    queue.tryPop();
    queue.pushRetry(restamped, 0ms);
    check(restamped->queueReadyTime() >= first,
          "Re-queueing the same job refreshes its ready time");
}

class SlotCollector final : public jobq::ExecutionObserver {
public:
    explicit SlotCollector(std::size_t workers) : slots_(workers) {}

    void onExecutionFinished(
        const jobq::ExecutionSample& sample
    ) noexcept override {
        auto& slot = slots_[sample.workerId - 1];
        slot.attempts.emplace_back(sample.jobId, sample.attempt);
        slot.queueWait.push_back(sample.queueWait);
        slot.execution.push_back(sample.executionDuration);
        (sample.succeeded ? slot.successes : slot.failures)++;
    }

    struct Slot {
        std::vector<std::pair<std::uint64_t, std::size_t>> attempts;
        std::vector<std::chrono::steady_clock::duration> queueWait;
        std::vector<std::chrono::steady_clock::duration> execution;
        std::size_t successes{0};
        std::size_t failures{0};
    };

    std::vector<Slot>& slots() { return slots_; }

private:
    std::vector<Slot> slots_;
};

void testObserverAccounting() {
    std::cout << "\n--- Execution Observer Accounting (real WorkerPool) ---\n";

    constexpr std::size_t kWorkers = 4;
    constexpr int kGood = 200;
    constexpr int kBad = 50;
    constexpr int kFlaky = 20;

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    SlotCollector collector(kWorkers);

    executor.registerHandler("GOOD", [](const jobq::Job&) {});
    executor.registerHandler("BAD", [](const jobq::Job&) {
        throw std::runtime_error("expected failure");
    });
    executor.registerHandler("FLAKY", [](const jobq::Job& job) {
        if (job.getAttemptsMade() == 1) {
            throw std::runtime_error("first attempt fails");
        }
    });

    {
        jobq::WorkerPool pool(queue, executor, kWorkers, metrics, collector);
        pool.start();

        jobq::RetryPolicy retry;
        retry.maxRetries = 1;
        retry.initialDelay = 1ms;
        retry.maxDelay = 1ms;

        for (int i = 0; i < kGood; ++i) {
            manager.submit("GOOD", "p");
        }
        for (int i = 0; i < kBad; ++i) {
            manager.submit("BAD", "p");
        }
        for (int i = 0; i < kFlaky; ++i) {
            manager.submit("FLAKY", "p", jobq::JobPriority::Normal, retry);
        }

        pool.stop();
    }

    LatencySamples queueWait;
    LatencySamples execution;
    std::set<std::pair<std::uint64_t, std::size_t>> attempts;
    std::size_t successes = 0;
    std::size_t failures = 0;
    std::size_t sampleCount = 0;
    bool nonNegative = true;

    for (const auto& slot : collector.slots()) {
        successes += slot.successes;
        failures += slot.failures;
        sampleCount += slot.attempts.size();
        attempts.insert(slot.attempts.begin(), slot.attempts.end());

        for (const auto& wait : slot.queueWait) {
            nonNegative = nonNegative && wait >= wait.zero();
            queueWait.add(
                std::chrono::duration_cast<ns>(wait)
            );
        }
        for (const auto& duration : slot.execution) {
            nonNegative = nonNegative && duration >= duration.zero();
            execution.add(
                std::chrono::duration_cast<ns>(duration)
            );
        }
    }

    const auto snapshot = metrics.snapshot();
    constexpr std::size_t expectedAttempts = kGood + kBad + 2 * kFlaky;

    check(snapshot.submittedJobs == kGood + kBad + kFlaky,
          "Unique submitted jobs are separate from attempts");
    check(sampleCount == expectedAttempts &&
              sampleCount == snapshot.startedExecutions &&
              sampleCount == snapshot.finishedExecutions,
          "One sample per started and finished execution attempt");
    check(attempts.size() == sampleCount,
          "No (job, attempt) pair is sampled twice");
    check(successes == snapshot.completedExecutions &&
              failures == snapshot.failedExecutions,
          "Successful and failed samples match completed and failed counts");
    check(successes == kGood + kFlaky && failures == kBad + kFlaky,
          "Retried jobs contribute one failed and one successful attempt");
    check(queueWait.count() == sampleCount && execution.count() == sampleCount,
          "Every sample has a queue wait and an execution duration");
    check(nonNegative, "All collected durations are non-negative");
    check(execution.total() == snapshot.totalExecutionTime,
          "Sum of execution samples equals JobMetrics total execution time");
    check(attempts.count({1, 1}) == 1 && !attempts.empty(),
          "Attempt numbers start at 1");
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobQ Latency Statistics Tests\n";
    std::cout << "========================================\n";

    testEmpty();
    testSingleSample();
    testOddAndEven();
    testPercentileConvention();
    testBoundaryValues();
    testAggregation();
    testConcurrentCollection();
    testQueueReadyTimestamps();
    testObserverAccounting();

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
