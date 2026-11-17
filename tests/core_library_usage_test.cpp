#include <array>
#include <atomic>
#include <iostream>
#include <span>

#include "executor/JobExecutor.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

int main() {
    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;
    std::atomic<unsigned> executions{0};

    executor.registerHandler("SMOKE", [&](const jobq::Job& job) {
        if (job.getPayload() == "library consumer") {
            executions.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // std::span also checks the library's transitive C++20 requirement.
    constexpr std::array priorities{
        jobq::JobPriority::Low,
        jobq::JobPriority::Normal,
        jobq::JobPriority::High
    };
    for (const auto priority : std::span(priorities)) {
        manager.submit("SMOKE", "library consumer", priority);
    }

    jobq::WorkerPool workers(queue, executor, 2, metrics);
    workers.start();
    workers.stop();

    const auto snapshot = metrics.snapshot();
    if (executions.load() != priorities.size() ||
        manager.totalJobs() != priorities.size() ||
        snapshot.submittedJobs != priorities.size() ||
        snapshot.startedExecutions != priorities.size() ||
        snapshot.completedExecutions != priorities.size() ||
        snapshot.finishedExecutions != priorities.size() ||
        snapshot.failedExecutions != 0 ||
        !queue.empty() || !workers.isStopped()) {
        std::cerr << "Core library consumer failed execution accounting\n";
        return 1;
    }

    for (const auto id : {1ULL, 2ULL, 3ULL}) {
        if (manager.getStatus(id) != jobq::JobStatus::Completed) {
            std::cerr << "Core library consumer found an incomplete job\n";
            return 1;
        }
    }

    std::cout << "Core library consumer passed\n";
    return 0;
}
