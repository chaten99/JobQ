#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "BenchmarkCommon.h"
#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/ExecutionObserver.h"
#include "metrics/JobMetrics.h"
#include "metrics/LatencyStatistics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace jobq_bench {

enum class StressScenario {
    SingleProducer,
    Concurrent,
    Sustained,
    Priority,
    Variable,
    Failure,
    Large
};

inline const char* stressScenarioName(StressScenario scenario) {
    switch (scenario) {
        case StressScenario::SingleProducer: return "single-producer";
        case StressScenario::Concurrent: return "concurrent";
        case StressScenario::Sustained: return "sustained";
        case StressScenario::Priority: return "priority";
        case StressScenario::Variable: return "variable";
        case StressScenario::Failure: return "failure";
        case StressScenario::Large: return "large";
    }
    return "unknown";
}

inline std::optional<StressScenario> parseStressScenario(std::string_view text) {
    for (const auto scenario : {
             StressScenario::SingleProducer,
             StressScenario::Concurrent,
             StressScenario::Sustained,
             StressScenario::Priority,
             StressScenario::Variable,
             StressScenario::Failure,
             StressScenario::Large}) {
        if (text == stressScenarioName(scenario)) {
            return scenario;
        }
    }

    return std::nullopt;
}

struct StressConfig {
    StressScenario scenario{StressScenario::Concurrent};
    Scenario workload{Scenario::Noop};
    std::uint64_t producers{4};
    std::uint64_t workers{4};
    bool collectLatency{true};
};

struct JobPlan {
    const char* type{""};
    jobq::JobPriority priority{jobq::JobPriority::Normal};
    jobq::RetryPolicy retry{};
    std::uint32_t attempts{1};
    std::uint32_t failedAttempts{0};
    std::uint32_t retries{0};
    bool completes{true};
};

inline jobq::RetryPolicy stressRetryPolicy() {
    jobq::RetryPolicy policy;
    policy.maxRetries = 1;
    policy.initialDelay = std::chrono::milliseconds(1);
    policy.multiplier = 2.0;
    policy.maxDelay = std::chrono::milliseconds(1);
    return policy;
}

inline Scenario effectiveWorkload(const StressConfig& config) {
    return config.scenario == StressScenario::Variable ? Scenario::Mixed
                                                       : config.workload;
}

inline jobq::JobPriority priorityForIndex(std::uint64_t index) {
    const auto slot = index % 10;

    if (slot < 2) {
        return jobq::JobPriority::High;
    }

    if (slot < 7) {
        return jobq::JobPriority::Normal;
    }

    return jobq::JobPriority::Low;
}

inline JobPlan planFor(
    const StressConfig& config,
    const std::vector<Kind>& kinds,
    std::uint64_t index
) {
    JobPlan plan;

    if (config.scenario == StressScenario::Failure) {
        const auto slot = index % 10;

        if (slot < 7) {
            plan.type = "STRESS_OK";
            return plan;
        }

        plan.retry = stressRetryPolicy();
        plan.attempts = 2;
        plan.retries = 1;

        if (slot < 9) {
            plan.type = "STRESS_FLAKY";
            plan.failedAttempts = 1;
            return plan;
        }

        plan.type = "STRESS_BAD";
        plan.failedAttempts = 2;
        plan.completes = false;
        return plan;
    }

    plan.type = kinds[kindIndexFor(effectiveWorkload(config), index)].type;

    if (config.scenario == StressScenario::Priority) {
        plan.priority = priorityForIndex(index);
    }

    return plan;
}

struct StressResult {
    bool ok{false};
    std::string error;

    std::uint64_t requested{0};
    std::uint64_t attempted{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected{0};

    std::uint64_t started{0};
    std::uint64_t finished{0};
    std::uint64_t completedAttempts{0};
    std::uint64_t failedAttempts{0};
    std::uint64_t scheduledRetries{0};

    std::uint64_t expectedAttempts{0};
    std::uint64_t expectedFailedAttempts{0};
    std::uint64_t expectedRetries{0};

    std::uint64_t finalCompleted{0};
    std::uint64_t finalFailed{0};
    std::uint64_t expectedFinalFailed{0};

    std::uint64_t uniqueIds{0};
    std::uint64_t registrySize{0};
    std::size_t finalQueueSize{0};
    bool queueEmpty{false};
    bool poolStopped{false};

    double seconds{0.0};
    double producerSeconds{0.0};
    double throughput{0.0};
    double submissionRate{0.0};

    double metricsAverageUs{0.0};

    bool latencyCollected{false};
    jobq::LatencySamples queueWait;
    jobq::LatencySamples executionSuccess;
    jobq::LatencySamples executionFailure;

    bool priorityCollected{false};
    std::array<jobq::LatencySamples, 3> priorityWait;
    std::array<std::uint64_t, 3> priorityAccepted{0, 0, 0};

    bool depthSampled{false};
    std::size_t depthSamples{0};
    std::size_t depthMax{0};
};

namespace stress_detail {

inline thread_local std::uint64_t lastJobId = 0;
inline thread_local int lastPriority = 1;

class Gate {
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

struct alignas(64) ProducerState {
    std::uint64_t attempted{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected{0};
    std::string firstError;
    SteadyClock::time_point finishedAt{};

    struct Entry {
        std::shared_ptr<jobq::Job> job;
        std::uint64_t index{0};
    };

    std::vector<Entry> entries;
};

class ThreadJoiner {
public:
    explicit ThreadJoiner(std::vector<std::thread>& threads)
        : threads_(threads) {}

    ~ThreadJoiner() {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::vector<std::thread>& threads_;
};

class DepthMonitor {
public:
    explicit DepthMonitor(jobq::JobQueue& queue) : queue_(queue) {
        thread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                const auto depth = queue_.size();
                ++samples_;
                maximum_ = std::max(maximum_, depth);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }

    ~DepthMonitor() {
        finish();
    }

    DepthMonitor(const DepthMonitor&) = delete;
    DepthMonitor& operator=(const DepthMonitor&) = delete;

    void finish() {
        stop_.store(true, std::memory_order_release);

        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::size_t samples() const { return samples_; }
    std::size_t maximum() const { return maximum_; }

private:
    jobq::JobQueue& queue_;
    std::atomic<bool> stop_{false};
    std::size_t samples_{0};
    std::size_t maximum_{0};
    std::thread thread_;
};

class StressObserver final : public jobq::ExecutionObserver {
public:
    StressObserver(
        std::size_t workers,
        std::size_t expectedJobs,
        bool trackPriority
    )
        : collector_(workers, expectedJobs, nullptr),
          priorityEnabled_(trackPriority),
          slots_(workers) {}

    void onExecutionFinished(
        const jobq::ExecutionSample& sample
    ) noexcept override {
        collector_.onExecutionFinished(sample);

        if (!priorityEnabled_) {
            return;
        }

        if (sample.workerId == 0 || sample.workerId > slots_.size()) {
            return;
        }

        PrioritySlot& slot = slots_[sample.workerId - 1];

        if (lastJobId != sample.jobId || lastPriority < 0 || lastPriority > 2) {
            ++slot.invalid;
            return;
        }

        try {
            slot.wait[static_cast<std::size_t>(lastPriority)].add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sample.queueWait
                )
            );
        } catch (...) {
            ++slot.invalid;
        }
    }

    std::uint64_t collect(StressResult& result) {
        std::uint64_t invalid = collector_.collect(
            result.queueWait,
            result.executionSuccess,
            result.executionFailure,
            discardedEndToEnd_
        );

        for (auto& slot : slots_) {
            for (std::size_t i = 0; i < 3; ++i) {
                result.priorityWait[i].merge(slot.wait[i]);
            }
            invalid += slot.invalid;
        }

        return invalid;
    }

private:
    struct alignas(64) PrioritySlot {
        std::array<jobq::LatencySamples, 3> wait;
        std::uint64_t invalid{0};
    };

    LatencyCollector collector_;
    bool priorityEnabled_;
    std::vector<PrioritySlot> slots_;
    jobq::LatencySamples discardedEndToEnd_;
};

inline void shutdownWithTimeout(
    jobq::WorkerPool& pool,
    std::chrono::seconds timeout
) {
    auto stopped = std::async(std::launch::async, [&pool] { pool.stop(); });

    if (stopped.wait_for(timeout) != std::future_status::ready) {
        std::fputs(
            "FATAL: worker pool shutdown did not complete within the "
            "timeout; terminating because runtime objects cannot be "
            "destroyed safely while workers are still active\n",
            stderr
        );
        std::fflush(stderr);
        std::_Exit(3);
    }

    stopped.get();
}

}

inline StressResult runStress(const StressConfig& config, std::uint64_t jobs) {
    using namespace stress_detail;

    StressResult result;
    result.requested = jobs;

    const std::uint64_t producerCount =
        config.scenario == StressScenario::SingleProducer ? 1
                                                          : config.producers;
    const bool failureScenario = config.scenario == StressScenario::Failure;
    const bool priorityScenario = config.scenario == StressScenario::Priority;
    const bool sampleDepth = config.scenario == StressScenario::Sustained;
    const auto workload = effectiveWorkload(config);
    const auto kinds = kindsFor(workload);

    StreamSilencer silencer;

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;

    std::vector<std::atomic<std::uint32_t>> executions(
        static_cast<std::size_t>(jobs) + 2
    );
    std::atomic<std::uint64_t> outOfRangeExecutions{0};
    std::atomic<std::uint64_t> checksum{0};

    const auto record = [&executions, &outOfRangeExecutions](
                            const jobq::Job& job
                        ) {
        lastJobId = job.getId();
        lastPriority = static_cast<int>(job.getPriority());

        if (job.getId() < executions.size()) {
            executions[static_cast<std::size_t>(job.getId())].fetch_add(
                1, std::memory_order_relaxed
            );
        } else {
            outOfRangeExecutions.fetch_add(1, std::memory_order_relaxed);
        }
    };

    if (failureScenario) {
        executor.registerHandler("STRESS_OK", record);
        executor.registerHandler(
            "STRESS_FLAKY",
            [record](const jobq::Job& job) {
                record(job);

                if (job.getAttemptsMade() == 1) {
                    throw std::runtime_error("deterministic first-attempt failure");
                }
            }
        );
        executor.registerHandler(
            "STRESS_BAD",
            [record](const jobq::Job& job) {
                record(job);
                throw std::runtime_error("deterministic permanent failure");
            }
        );
    } else {
        for (const auto& kind : kinds) {
            const Work work = kind.work;
            executor.registerHandler(
                kind.type,
                [record, &checksum, work](const jobq::Job& job) {
                    record(job);
                    checksum.fetch_add(
                        runWork(work, job.getId()), std::memory_order_relaxed
                    );
                }
            );
        }
    }

    std::unique_ptr<StressObserver> observer;
    std::unique_ptr<jobq::WorkerPool> pool;

    try {
        if (config.collectLatency) {
            observer = std::make_unique<StressObserver>(
                static_cast<std::size_t>(config.workers),
                static_cast<std::size_t>(jobs),
                priorityScenario
            );
            pool = std::make_unique<jobq::WorkerPool>(
                queue,
                executor,
                static_cast<std::size_t>(config.workers),
                metrics,
                *observer
            );
        } else {
            pool = std::make_unique<jobq::WorkerPool>(
                queue,
                executor,
                static_cast<std::size_t>(config.workers),
                metrics
            );
        }

        pool->start();
    } catch (const std::exception& exception) {
        result.error =
            std::string("worker pool failed to start: ") + exception.what();
        return result;
    }

    std::vector<ProducerState> producers(static_cast<std::size_t>(producerCount));
    Gate gate;
    std::vector<std::thread> threads;
    std::unique_ptr<DepthMonitor> monitor;

    const std::uint64_t base = jobs / producerCount;
    const std::uint64_t remainder = jobs % producerCount;

    std::string startupError;
    SteadyClock::time_point startTime{};

    {
        ThreadJoiner joiner(threads);

        try {
            threads.reserve(static_cast<std::size_t>(producerCount));

            for (std::uint64_t p = 0; p < producerCount; ++p) {
                const std::uint64_t first =
                    p * base + std::min(p, remainder);
                const std::uint64_t count = base + (p < remainder ? 1 : 0);

                threads.emplace_back([&, p, first, count] {
                    ProducerState& state = producers[static_cast<std::size_t>(p)];
                    state.entries.reserve(static_cast<std::size_t>(count));

                    gate.wait();

                    for (std::uint64_t i = first; i < first + count; ++i) {
                        const JobPlan plan = planFor(config, kinds, i);
                        ++state.attempted;

                        try {
                            auto job = manager.submit(
                                plan.type, "payload", plan.priority, plan.retry
                            );
                            ++state.accepted;
                            state.entries.push_back({std::move(job), i});
                        } catch (const std::exception& exception) {
                            ++state.rejected;

                            if (state.firstError.empty()) {
                                state.firstError = exception.what();
                            }
                        }
                    }

                    state.finishedAt = SteadyClock::now();
                });
            }

            if (sampleDepth) {
                monitor = std::make_unique<DepthMonitor>(queue);
            }
        } catch (const std::exception& exception) {
            startupError =
                std::string("producer thread creation failed: ") +
                exception.what();
        }

        startTime = SteadyClock::now();
        gate.release();
    }

    SteadyClock::time_point producersDone = startTime;

    for (const auto& state : producers) {
        if (state.finishedAt > producersDone) {
            producersDone = state.finishedAt;
        }
    }

    const auto timeout = std::chrono::seconds(60 + jobs / 5000);

    try {
        shutdownWithTimeout(*pool, timeout);
    } catch (const std::exception& exception) {
        result.error = std::string("worker pool failed to stop: ") +
                       exception.what();
        return result;
    }

    const auto endTime = SteadyClock::now();

    if (monitor) {
        monitor->finish();
        result.depthSampled = true;
        result.depthSamples = monitor->samples();
        result.depthMax = monitor->maximum();
    }

    result.seconds = std::chrono::duration<double>(endTime - startTime).count();
    result.producerSeconds =
        std::chrono::duration<double>(producersDone - startTime).count();

    std::vector<std::string> problems;

    if (!startupError.empty()) {
        problems.push_back(startupError);
    }

    std::vector<std::uint64_t> ids;
    std::vector<const ProducerState::Entry*> accepted;

    for (const auto& state : producers) {
        result.attempted += state.attempted;
        result.accepted += state.accepted;
        result.rejected += state.rejected;

        if (!state.firstError.empty()) {
            problems.push_back("submission rejected: " + state.firstError);
        }

        for (const auto& entry : state.entries) {
            accepted.push_back(&entry);
            ids.push_back(entry.job->getId());
        }
    }

    const auto snapshot = metrics.snapshot();

    result.started = snapshot.startedExecutions;
    result.finished = snapshot.finishedExecutions;
    result.completedAttempts = snapshot.completedExecutions;
    result.failedAttempts = snapshot.failedExecutions;
    result.scheduledRetries = snapshot.scheduledRetries;
    result.metricsAverageUs =
        std::chrono::duration<double, std::micro>(
            snapshot.averageExecutionTime
        ).count();
    result.finalQueueSize = queue.size();
    result.queueEmpty = queue.empty();
    result.poolStopped = pool->isStopped();
    result.registrySize = manager.totalJobs();

    for (std::uint64_t i = 0; i < jobs; ++i) {
        const JobPlan plan = planFor(config, kinds, i);
        result.expectedAttempts += plan.attempts;
        result.expectedFailedAttempts += plan.failedAttempts;
        result.expectedRetries += plan.retries;

        if (!plan.completes) {
            ++result.expectedFinalFailed;
        }
    }

    if (result.attempted != jobs) {
        problems.push_back(
            "attempted " + std::to_string(result.attempted) + " of " +
            std::to_string(jobs) + " submissions"
        );
    }
    if (result.accepted + result.rejected != result.attempted) {
        problems.push_back("accepted + rejected != attempted");
    }
    if (result.rejected != 0) {
        problems.push_back(
            std::to_string(result.rejected) + " submission(s) rejected"
        );
    }

    std::sort(ids.begin(), ids.end());
    const bool duplicateIds =
        std::adjacent_find(ids.begin(), ids.end()) != ids.end();
    result.uniqueIds = duplicateIds
        ? std::unique(ids.begin(), ids.end()) - ids.begin()
        : ids.size();

    if (duplicateIds) {
        problems.push_back("duplicate job IDs among accepted jobs");
    }
    if (result.rejected == 0 && !ids.empty() &&
        (ids.front() != 1 || ids.back() != ids.size())) {
        problems.push_back("accepted job IDs are not the contiguous range 1..N");
    }
    if (result.registrySize != result.accepted) {
        problems.push_back(
            "registry holds " + std::to_string(result.registrySize) +
            " jobs but " + std::to_string(result.accepted) + " were accepted"
        );
    }

    std::uint64_t registryMismatches = 0;
    std::uint64_t wrongStatus = 0;
    std::uint64_t wrongExecutions = 0;
    std::uint64_t expectedChecksum = 0;

    for (const auto* entry : accepted) {
        const auto& job = entry->job;
        const JobPlan plan = planFor(config, kinds, entry->index);

        if (manager.getJob(job->getId()) != job) {
            ++registryMismatches;
        }

        const auto expectedStatus = plan.completes ? jobq::JobStatus::Completed
                                                   : jobq::JobStatus::Failed;

        if (job->getStatus() != expectedStatus) {
            ++wrongStatus;
        } else if (plan.completes) {
            ++result.finalCompleted;
        } else {
            ++result.finalFailed;
        }

        if (job->getId() >= executions.size() ||
            executions[static_cast<std::size_t>(job->getId())].load() !=
                plan.attempts) {
            ++wrongExecutions;
        }

        if (priorityScenario) {
            ++result.priorityAccepted[static_cast<std::size_t>(plan.priority)];
        }

        if (!failureScenario) {
            for (const auto& kind : kinds) {
                if (std::string_view(kind.type) == plan.type) {
                    expectedChecksum += runWork(kind.work, job->getId());
                    break;
                }
            }
        }
    }

    if (registryMismatches != 0) {
        problems.push_back(
            std::to_string(registryMismatches) +
            " accepted job(s) missing from or different in the registry"
        );
    }
    if (wrongStatus != 0) {
        problems.push_back(
            std::to_string(wrongStatus) +
            " accepted job(s) not in their expected final state"
        );
    }
    if (wrongExecutions != 0 || outOfRangeExecutions.load() != 0) {
        problems.push_back(
            std::to_string(wrongExecutions) +
            " job(s) executed a wrong number of times (duplicate or missing "
            "execution)"
        );
    }
    if (!failureScenario && checksum.load() != expectedChecksum) {
        problems.push_back("workload checksum mismatch");
    }

    if (snapshot.submittedJobs != result.accepted) {
        problems.push_back("metrics submitted != accepted jobs");
    }
    if (result.started != result.expectedAttempts ||
        result.finished != result.started) {
        problems.push_back(
            "execution attempts: expected " +
            std::to_string(result.expectedAttempts) + ", started " +
            std::to_string(result.started) + ", finished " +
            std::to_string(result.finished)
        );
    }
    if (result.failedAttempts != result.expectedFailedAttempts ||
        result.completedAttempts + result.failedAttempts != result.finished) {
        problems.push_back(
            "failed attempts: expected " +
            std::to_string(result.expectedFailedAttempts) + ", got " +
            std::to_string(result.failedAttempts)
        );
    }
    if (result.scheduledRetries != result.expectedRetries) {
        problems.push_back(
            "scheduled retries: expected " +
            std::to_string(result.expectedRetries) + ", got " +
            std::to_string(result.scheduledRetries)
        );
    }
    if (result.finalFailed != result.expectedFinalFailed) {
        problems.push_back("permanently failed job count differs from plan");
    }
    if (!result.queueEmpty || result.finalQueueSize != 0) {
        problems.push_back("queue not empty after shutdown");
    }
    if (!result.poolStopped) {
        problems.push_back("worker pool not stopped");
    }

    if (config.collectLatency) {
        result.latencyCollected = true;
        result.priorityCollected = priorityScenario;

        const std::uint64_t invalid = observer->collect(result);

        if (invalid != 0) {
            problems.push_back(
                std::to_string(invalid) + " invalid latency sample(s)"
            );
        }
        if (result.queueWait.count() != result.started) {
            problems.push_back("queue wait samples != started executions");
        }
        if (result.executionSuccess.count() != result.completedAttempts ||
            result.executionFailure.count() != result.failedAttempts) {
            problems.push_back(
                "execution samples do not match completed/failed attempts"
            );
        }

        const auto okTotal = result.executionSuccess.total();
        const auto failTotal = result.executionFailure.total();

        if (!okTotal || !failTotal ||
            *okTotal + *failTotal != snapshot.totalExecutionTime) {
            problems.push_back(
                "execution samples do not sum to the JobMetrics total"
            );
        }

        if (priorityScenario) {
            std::size_t prioritySamples = 0;
            for (const auto& samples : result.priorityWait) {
                prioritySamples += samples.count();
            }
            if (prioritySamples != result.started) {
                problems.push_back("per-priority wait samples != started executions");
            }
        }
    }

    if (!problems.empty()) {
        for (std::size_t i = 0; i < problems.size(); ++i) {
            result.error += (i ? "; " : "") + problems[i];
        }
        return result;
    }

    if (result.seconds <= 0.0) {
        result.error = "non-positive benchmark duration";
        return result;
    }

    result.throughput =
        static_cast<double>(result.finalCompleted) / result.seconds;

    if (result.producerSeconds > 0.0) {
        result.submissionRate =
            static_cast<double>(result.accepted) / result.producerSeconds;
    }

    result.ok = true;
    return result;
}

}
