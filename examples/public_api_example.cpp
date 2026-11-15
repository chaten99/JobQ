#include <atomic>
#include <iostream>

#include "jobq/Engine.h"

int main() {
    std::atomic<unsigned> processed{0};
    jobq::Engine engine({.workerCount = 2});

    engine.registerHandler("EMAIL", [&](const jobq::Job& job) {
        std::cout << "Sending email to " << job.getPayload() << '\n';
        ++processed;
    });

    engine.start();
    const auto id = engine.submit(
        "EMAIL", "reader@example.com", {.priority = jobq::JobPriority::High}
    );
    engine.stop();

    if (engine.getStatus(id) != jobq::JobStatus::Completed || processed != 1) {
        return 1;
    }
    std::cout << "Completed job " << id << ", total completed "
              << engine.metrics().completedExecutions << '\n';
    return 0;
}
