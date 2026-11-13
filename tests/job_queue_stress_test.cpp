#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "StressRunner.h"
#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

using namespace jobq_bench;
using jobq::JobPriority;
using jobq::JobStatus;

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

class StartGate {
public:
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_{false};
};

struct SubmitOutcome {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::mutex mutex;
    std::vector<std::shared_ptr<jobq::Job>> jobs;
};

void runProducers(
    jobq::JobManager& manager,
    std::size_t producers,
    std::size_t perProducer,
    SubmitOutcome& outcome,
    JobPriority (*priorityFor)(std::size_t, std::size_t)
) {
    StartGate gate;
    std::vector<std::thread> threads;

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            std::vector<std::shared_ptr<jobq::Job>> local;
            local.reserve(perProducer);
            gate.wait();

            for (std::size_t i = 0; i < perProducer; ++i) {
                try {
                    local.push_back(
                        manager.submit("T", "p", priorityFor(p, i))
                    );
                    outcome.accepted.fetch_add(1);
                } catch (const std::exception&) {
                    outcome.rejected.fetch_add(1);
                }
            }

            std::lock_guard<std::mutex> lock(outcome.mutex);
            outcome.jobs.insert(outcome.jobs.end(), local.begin(), local.end());
        });
    }

    gate.release();

    for (auto& thread : threads) {
        thread.join();
    }
}

JobPriority normalPriority(std::size_t, std::size_t) {
    return JobPriority::Normal;
}

void testConcurrentProducers() {
    std::cout << "\n--- Test A: Concurrent Producers ---\n";

    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 2500;

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    SubmitOutcome outcome;

    runProducers(manager, kProducers, kPerProducer, outcome, normalPriority);

    const std::uint64_t attempted = kProducers * kPerProducer;

    check(outcome.accepted + outcome.rejected == attempted,
          "Every submission is exactly accepted or rejected");
    check(outcome.rejected == 0 && outcome.accepted == attempted,
          "All submissions to an open queue are accepted");

    std::set<std::uint64_t> ids;
    for (const auto& job : outcome.jobs) {
        ids.insert(job->getId());
    }
    check(ids.size() == attempted, "Accepted jobs have unique IDs");
    check(*ids.begin() == 1 && *ids.rbegin() == attempted,
          "IDs form the contiguous range 1..N");
    check(manager.totalJobs() == attempted, "Registry holds every accepted job");
    check(queue.size() == attempted, "Every accepted job is queued exactly once");
    check(metrics.snapshot().submittedJobs == attempted,
          "Metrics count exactly the accepted submissions");

    std::set<std::uint64_t> drained;
    while (auto job = queue.tryPop()) {
        drained.insert(job->getId());
    }
    check(drained == ids, "Draining the queue yields exactly the accepted jobs");
}

void testConcurrentConsumers() {
    std::cout << "\n--- Test B: Concurrent Consumers ---\n";

    constexpr std::size_t kJobs = 8000;
    constexpr std::size_t kWorkers = 6;

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);

    std::vector<std::atomic<std::uint32_t>> executions(kJobs + 2);
    executor.registerHandler("T", [&executions](const jobq::Job& job) {
        executions[static_cast<std::size_t>(job.getId())].fetch_add(1);
    });

    std::vector<std::shared_ptr<jobq::Job>> jobs;
    for (std::size_t i = 0; i < kJobs; ++i) {
        jobs.push_back(manager.submit("T", "p"));
    }

    {
        jobq::WorkerPool pool(queue, executor, kWorkers, metrics);
        pool.start();
        pool.stop();
        check(pool.isStopped(), "Worker pool stopped and joined");
    }

    std::size_t wrong = 0;
    std::size_t notCompleted = 0;
    for (const auto& job : jobs) {
        wrong += executions[static_cast<std::size_t>(job->getId())].load() != 1;
        notCompleted += job->getStatus() != JobStatus::Completed;
    }

    check(wrong == 0, "Every job was executed exactly once (no loss, no duplicate)");
    check(notCompleted == 0, "Every job reached Completed");
    check(queue.empty(), "Queue is empty after shutdown");

    const auto snapshot = metrics.snapshot();
    check(snapshot.completedExecutions == kJobs && snapshot.failedExecutions == 0 &&
              snapshot.startedExecutions == kJobs,
          "Metrics match the workload");
}

void testSustainedSubmission() {
    std::cout << "\n--- Test C: Sustained Submission ---\n";

    StressConfig config;
    config.scenario = StressScenario::Sustained;
    config.producers = 4;
    config.workers = 4;

    const auto result = runStress(config, 8000);

    check(result.ok, "Sustained run passes its integrity checks" +
                         (result.ok ? std::string() : " (" + result.error + ")"));
    check(result.attempted == 8000 && result.accepted == 8000 && result.rejected == 0,
          "All submissions accounted for");
    check(result.finalCompleted == result.accepted,
          "Every accepted job completed while producers were running");
    check(result.depthSampled && result.depthSamples > 0,
          "Queue depth was sampled during the run");
    check(result.producerSeconds <= result.seconds,
          "Producer phase is part of the total duration");
}

std::vector<std::uint64_t> g_order;
std::vector<int> g_priorityOrder;
std::mutex g_orderMutex;

JobPriority mixedPriority(std::size_t producer, std::size_t index) {
    switch ((producer + index) % 3) {
        case 0: return JobPriority::High;
        case 1: return JobPriority::Normal;
        default: return JobPriority::Low;
    }
}

void testMixedPriorities() {
    std::cout << "\n--- Test D: Mixed Priorities ---\n";

    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 1500;

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);

    g_order.clear();
    g_priorityOrder.clear();

    executor.registerHandler("T", [](const jobq::Job& job) {
        std::lock_guard<std::mutex> lock(g_orderMutex);
        g_order.push_back(job.getId());
        g_priorityOrder.push_back(static_cast<int>(job.getPriority()));
    });

    SubmitOutcome outcome;
    runProducers(manager, kProducers, kPerProducer, outcome, mixedPriority);

    check(outcome.accepted == kProducers * kPerProducer, "All jobs accepted before workers start");

    {
        jobq::WorkerPool pool(queue, executor, 1, metrics);
        pool.start();
        pool.stop();
    }

    check(g_order.size() == kProducers * kPerProducer,
          "A single worker executed every queued job");

    bool nonIncreasing = std::is_sorted(
        g_priorityOrder.begin(),
        g_priorityOrder.end(),
        [](int a, int b) { return a > b; }
    );
    check(nonIncreasing,
          "With everything queued first, all High jobs run before Normal before Low");

    jobq::JobQueue fifoQueue;
    jobq::JobExecutor fifoExecutor;
    jobq::JobManager fifoManager(fifoQueue);

    g_order.clear();
    g_priorityOrder.clear();
    fifoExecutor.registerHandler("T", [](const jobq::Job& job) {
        std::lock_guard<std::mutex> lock(g_orderMutex);
        g_order.push_back(job.getId());
        g_priorityOrder.push_back(static_cast<int>(job.getPriority()));
    });

    for (std::size_t i = 0; i < 3000; ++i) {
        fifoManager.submit("T", "p", mixedPriority(0, i));
    }

    {
        jobq::WorkerPool pool(fifoQueue, fifoExecutor, 1);
        pool.start();
        pool.stop();
    }

    bool fifoWithinPriority = true;
    std::uint64_t lastId[3] = {0, 0, 0};
    for (std::size_t i = 0; i < g_order.size(); ++i) {
        const auto priority = static_cast<std::size_t>(g_priorityOrder[i]);
        fifoWithinPriority = fifoWithinPriority && g_order[i] > lastId[priority];
        lastId[priority] = g_order[i];
    }
    check(g_order.size() == 3000 && fifoWithinPriority,
          "A single producer's jobs run in submission order within each priority");

    StressConfig config;
    config.scenario = StressScenario::Priority;
    config.producers = 4;
    config.workers = 4;
    const auto result = runStress(config, 6000);
    check(result.ok, "Priority stress run passes its integrity checks" +
                         (result.ok ? std::string() : " (" + result.error + ")"));
    check(result.priorityAccepted[2] == 1200 && result.priorityAccepted[1] == 3000 &&
              result.priorityAccepted[0] == 1800,
          "Priority distribution is exactly 20% High, 50% Normal, 30% Low");
}

void testQueueDrain() {
    std::cout << "\n--- Test E: Queue Drain ---\n";

    StressConfig config;
    config.scenario = StressScenario::Failure;
    config.producers = 3;
    config.workers = 4;

    const auto result = runStress(config, 5000);

    check(result.ok, "Failure/retry run passes its integrity checks" +
                         (result.ok ? std::string() : " (" + result.error + ")"));
    check(result.queueEmpty && result.finalQueueSize == 0 && result.poolStopped,
          "Queue is empty and the pool stopped after draining, including retries");
    check(result.finalCompleted + result.finalFailed == result.accepted,
          "Every accepted job reached a terminal state");
    check(result.finalCompleted == 4500 && result.finalFailed == 500,
          "Deterministic failure mix: 90% complete, 10% permanently fail");
}

void testMetricsConsistency() {
    std::cout << "\n--- Test F: Metrics Consistency ---\n";

    for (const auto scenario :
         {StressScenario::Concurrent, StressScenario::Variable, StressScenario::Failure}) {
        StressConfig config;
        config.scenario = scenario;
        config.workload = Scenario::Lightweight;
        config.producers = 4;
        config.workers = 3;

        const auto result = runStress(config, 4000);
        const std::string name = stressScenarioName(scenario);

        check(result.ok, name + ": integrity checks pass" +
                             (result.ok ? std::string() : " (" + result.error + ")"));
        check(result.started == result.finished &&
                  result.finished == result.completedAttempts + result.failedAttempts,
              name + ": started == finished == completed + failed attempts");
        check(result.scheduledRetries <= result.failedAttempts,
              name + ": scheduled retries never exceed failed attempts");
        check(result.started == result.expectedAttempts &&
                  result.failedAttempts == result.expectedFailedAttempts &&
                  result.scheduledRetries == result.expectedRetries,
              name + ": counters equal the deterministic plan");
        check(result.queueWait.count() == result.started &&
                  result.executionSuccess.count() == result.completedAttempts &&
                  result.executionFailure.count() == result.failedAttempts,
              name + ": latency samples match attempts");
    }
}

void testRepeatedRuns() {
    std::cout << "\n--- Test G: Repeated Stress Runs ---\n";

    int failures = 0;
    std::string firstError;

    const StressScenario scenarios[] = {
        StressScenario::SingleProducer,
        StressScenario::Concurrent,
        StressScenario::Priority,
        StressScenario::Failure,
    };

    for (int round = 0; round < 3; ++round) {
        for (const auto scenario : scenarios) {
            StressConfig config;
            config.scenario = scenario;
            config.producers = 4;
            config.workers = 2 + static_cast<std::uint64_t>(round);

            const auto result = runStress(config, 3000);

            if (!result.ok) {
                ++failures;
                if (firstError.empty()) {
                    firstError = std::string(stressScenarioName(scenario)) + ": " +
                                 result.error;
                }
            }
        }
    }

    check(failures == 0,
          "12 repeated runs: no loss, duplication, deadlock or inconsistent state" +
              (failures == 0 ? std::string() : " (" + firstError + ")"));
}


void testFreshManagerFirstUse() {
    std::cout << "\n--- Test H: Concurrent First Use Of A Fresh JobManager ---\n";

    constexpr int kRounds = 300;
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 20;

    bool allAccounted = true;

    for (int round = 0; round < kRounds; ++round) {
        jobq::JobQueue queue;
        jobq::JobManager manager(queue);
        StartGate gate;
        std::vector<std::thread> threads;

        for (std::size_t t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                gate.wait();
                for (std::size_t i = 0; i < kPerThread; ++i) {
                    manager.submit("T", "p");
                }
            });
        }

        gate.release();

        for (auto& thread : threads) {
            thread.join();
        }

        allAccounted = allAccounted &&
                       manager.totalJobs() == kThreads * kPerThread &&
                       queue.size() == kThreads * kPerThread;
    }

    check(allAccounted,
          "300 fresh managers, each hit by 4 simultaneous first-time submitters");
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "      JobQ Queue Stress Tests\n";
    std::cout << "========================================\n";

    testConcurrentProducers();
    testConcurrentConsumers();
    testSustainedSubmission();
    testMixedPriorities();
    testQueueDrain();
    testMetricsConsistency();
    testRepeatedRuns();
    testFreshManagerFirstUse();

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
