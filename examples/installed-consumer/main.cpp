#include <array>
#include <atomic>
#include <latch>
#include <span>

#include "jobq/Engine.h"

int main() {
    std::atomic<unsigned> processed{0};
    std::latch finished{2};
    jobq::Engine engine({.workerCount = 2});
    engine.registerHandler("TASK", [&](const jobq::Job& job) {
        if (job.getPayload() == "installed package") {
            ++processed;
        }
        finished.count_down();
    });

    engine.start();
    std::array<jobq::JobId, 2> ids{};
    for (auto& id : std::span(ids)) {
        id = engine.submit("TASK", "installed package");
    }
    finished.wait();
    engine.stop();

    for (const auto id : ids) {
        if (engine.getStatus(id) != jobq::JobStatus::Completed) {
            return 1;
        }
    }
    return processed == ids.size() &&
           engine.metrics().completedExecutions == ids.size() ? 0 : 1;
}
