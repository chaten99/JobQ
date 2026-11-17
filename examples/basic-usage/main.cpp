#include <chrono>
#include <future>
#include <iostream>
#include <string>

#include "jobq/Engine.h"

int main() {
    std::promise<std::string> delivered;
    auto result = delivered.get_future();
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("EMAIL", [&](const jobq::Job& job) {
        delivered.set_value(job.getPayload());
    });

    engine.start();
    const auto id = engine.submit("EMAIL", "reader@example.com");
    const auto ready = result.wait_for(std::chrono::seconds(5));
    engine.stop();

    if (ready != std::future_status::ready ||
        result.get() != "reader@example.com" ||
        engine.getStatus(id) != jobq::JobStatus::Completed) {
        return 1;
    }
    std::cout << "Delivered email job " << id << '\n';
    return 0;
}
