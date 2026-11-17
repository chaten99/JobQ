#include <atomic>
#include <iostream>
#include <string>

#include "jobq/Engine.h"

int main() {
    std::atomic<unsigned> emails{0};
    std::atomic<unsigned> reports{0};
    std::atomic<int> total{0};
    jobq::Engine engine({.workerCount = 3});
    engine.registerHandler("EMAIL", [&](const jobq::Job& job) {
        if (job.getPayload() == "reader@example.com") {
            ++emails;
        }
    });
    engine.registerHandler("REPORT", [&](const jobq::Job& job) {
        if (job.getPayload() == "monthly-sales") {
            ++reports;
        }
    });
    engine.registerHandler("CALCULATE", [&](const jobq::Job& job) {
        total += std::stoi(job.getPayload());
    });

    engine.start();
    const auto email = engine.submit("EMAIL", "reader@example.com");
    const auto report = engine.submit("REPORT", "monthly-sales");
    const auto calculation = engine.submit("CALCULATE", "42");
    engine.stop();

    if (emails != 1 || reports != 1 || total != 42 ||
        engine.metrics().completedExecutions != 3) {
        return 1;
    }
    for (const auto id : {email, report, calculation}) {
        if (engine.getStatus(id) != jobq::JobStatus::Completed) {
            return 1;
        }
    }
    std::cout << "Processed email, report, and calculation jobs\n";
    return 0;
}
