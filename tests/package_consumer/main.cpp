#include <array>
#include <atomic>
#include <span>

#include "jobq/Engine.h"

int main() {
    std::atomic<unsigned> processed{0};
    jobq::Engine engine({.workerCount = 2});
    engine.registerHandler("TASK", [&](const jobq::Job& job) {
        if (job.getPayload() == "package consumer") {
            ++processed;
        }
    });

    engine.start();
    std::array<jobq::JobId, 2> ids{};
    for (auto& id : std::span(ids)) {
        id = engine.submit("TASK", "package consumer");
    }
    engine.stop();

    for (const auto id : ids) {
        if (engine.getStatus(id) != jobq::JobStatus::Completed) {
            return 1;
        }
    }
    return processed == ids.size() &&
           engine.metrics().completedExecutions == ids.size() ? 0 : 1;
}
