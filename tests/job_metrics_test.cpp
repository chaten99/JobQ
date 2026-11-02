#include "metrics/JobMetrics.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

int testsRun = 0;
int testsFailed = 0;

void check(bool condition, const char* description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    std::cout << "========================================\n";
    std::cout << "     JobQ Runtime Metrics Tests\n";
    std::cout << "========================================\n\n";

    jobq::JobMetrics metrics;

    auto initial = metrics.snapshot();

    check(
        initial.submittedJobs == 0 &&
            initial.startedExecutions == 0 &&
            initial.completedExecutions == 0 &&
            initial.failedExecutions == 0 &&
            initial.scheduledRetries == 0,
        "New metrics object starts with zero counters"
    );

    check(
        initial.finishedExecutions == 0 &&
            initial.totalExecutionTime == 0ns &&
            initial.averageExecutionTime == 0ns,
        "New metrics object starts with zero execution totals"
    );

    check(
        initial.throughputJobsPerSecond == 0.0,
        "Throughput starts at zero when no execution has finished"
    );

    metrics.recordJobSubmitted();
    metrics.recordExecutionStarted();
    metrics.recordExecutionCompleted(10ms);

    metrics.recordJobSubmitted();
    metrics.recordExecutionStarted();
    metrics.recordExecutionFailed(20ms);
    metrics.recordRetryScheduled();

    auto recorded = metrics.snapshot();

    check(
        recorded.submittedJobs == 2,
        "Submission events are counted"
    );

    check(
        recorded.startedExecutions == 2,
        "Execution starts are counted"
    );

    check(
        recorded.completedExecutions == 1,
        "Successful execution attempts are counted"
    );

    check(
        recorded.failedExecutions == 1,
        "Failed execution attempts are counted"
    );

    check(
        recorded.scheduledRetries == 1,
        "Retry scheduling events are counted"
    );

    check(
        recorded.finishedExecutions == 2,
        "Finished execution count includes successes and failures"
    );

    check(
        recorded.totalExecutionTime == 30ms,
        "Total execution time is accumulated correctly"
    );

    check(
        recorded.averageExecutionTime == 15ms,
        "Average execution time is calculated correctly"
    );

    check(
        recorded.elapsedTime >= 0ns,
        "Elapsed runtime is non-negative"
    );

    std::this_thread::sleep_for(2ms);

    recorded = metrics.snapshot();

    check(
        std::isfinite(recorded.throughputJobsPerSecond) &&
            recorded.throughputJobsPerSecond > 0.0,
        "Throughput is calculated from finished executions and elapsed time"
    );

    bool negativeCompletionRejected = false;

    try {
        metrics.recordExecutionCompleted(-1ms);
    } catch (const std::invalid_argument&) {
        negativeCompletionRejected = true;
    }

    check(
        negativeCompletionRejected,
        "Negative completion duration is rejected"
    );

    bool negativeFailureRejected = false;

    try {
        metrics.recordExecutionFailed(-1ms);
    } catch (const std::invalid_argument&) {
        negativeFailureRejected = true;
    }

    check(
        negativeFailureRejected,
        "Negative failure duration is rejected"
    );

    const auto afterInvalidInputs = metrics.snapshot();

    check(
        afterInvalidInputs.completedExecutions == 1 &&
            afterInvalidInputs.failedExecutions == 1 &&
            afterInvalidInputs.totalExecutionTime == 30ms,
        "Invalid durations do not change metric counters or totals"
    );

    // Test 4: Concurrent updates.
    jobq::JobMetrics concurrentMetrics;

    constexpr std::size_t threadCount = 8;
    constexpr std::size_t operationsPerThread = 1000;

    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (std::size_t threadIndex = 0;
         threadIndex < threadCount;
         ++threadIndex) {
        workers.emplace_back([&concurrentMetrics]() {
            for (std::size_t operation = 0;
                 operation < operationsPerThread;
                 ++operation) {
                concurrentMetrics.recordJobSubmitted();
                concurrentMetrics.recordExecutionStarted();

                if (operation % 5 == 0) {
                    concurrentMetrics.recordRetryScheduled();
                }

                concurrentMetrics.recordExecutionCompleted(1ms);
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    const auto concurrent = concurrentMetrics.snapshot();

    constexpr std::uint64_t expectedExecutions =
        threadCount * operationsPerThread;

    constexpr std::uint64_t expectedRetries =
        threadCount * (operationsPerThread / 5);

    check(
        concurrent.submittedJobs == expectedExecutions,
        "Concurrent submissions are counted without lost updates"
    );

    check(
        concurrent.startedExecutions == expectedExecutions,
        "Concurrent execution starts are counted correctly"
    );

    check(
        concurrent.completedExecutions == expectedExecutions,
        "Concurrent completions are counted without lost updates"
    );

    check(
        concurrent.failedExecutions == 0,
        "Concurrent successful executions do not increment failures"
    );

    check(
        concurrent.scheduledRetries == expectedRetries,
        "Concurrent retry events are counted correctly"
    );

    check(
        concurrent.finishedExecutions == expectedExecutions,
        "Concurrent finished execution total is correct"
    );

    check(
        concurrent.totalExecutionTime ==
            std::chrono::milliseconds(expectedExecutions),
        "Concurrent execution durations accumulate correctly"
    );

    check(
        concurrent.averageExecutionTime == 1ms,
        "Concurrent average execution time is correct"
    );

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';

    std::cout
        << (testsFailed == 0
                ? "Result: ALL TESTS PASSED\n"
                : "Result: TEST FAILURES DETECTED\n");

    std::cout << "========================================\n";

    return testsFailed == 0 ? 0 : 1;
}