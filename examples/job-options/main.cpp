#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "jobq/Engine.h"

int main() {
    std::atomic<unsigned> attempts{0};
    std::atomic<bool> priorityMatches{true};
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("SYNC", [&](const jobq::Job& job) {
        if (job.getPriority() != jobq::JobPriority::High) {
            priorityMatches = false;
        }
        ++attempts;
        if (job.getAttemptsMade() == 1) {
            throw std::runtime_error("Temporary service failure");
        }
    });

    engine.start();
    const auto id = engine.submit("SYNC", "customer-42", {
        .priority = jobq::JobPriority::High,
        .retryPolicy = {
            .maxRetries = 2,
            .initialDelay = std::chrono::milliseconds(10),
            .multiplier = 2.0,
            .maxDelay = std::chrono::milliseconds(100)
        }
    });
    engine.stop();

    const auto metrics = engine.metrics();
    if (!priorityMatches || attempts != 2 || metrics.scheduledRetries != 1 ||
        metrics.failedExecutions != 1 || metrics.completedExecutions != 1 ||
        engine.getStatus(id) != jobq::JobStatus::Completed) {
        return 1;
    }
    std::cout << "Recovered sync job " << id << " after a delayed retry\n";
    return 0;
}
