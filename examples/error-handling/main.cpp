#include <iostream>
#include <stdexcept>

#include "jobq/Engine.h"

int main() {
    unsigned rejected{0};
    try {
        jobq::Engine invalid({.workerCount = 0});
    } catch (const std::invalid_argument& error) {
        std::cout << error.what() << '\n';
        ++rejected;
    }

    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("REPORT", [](const jobq::Job&) {
        throw std::runtime_error("Report input is unavailable");
    });
    try {
        engine.registerHandler("REPORT", [](const jobq::Job&) {});
    } catch (const std::invalid_argument& error) {
        std::cout << error.what() << '\n';
        ++rejected;
    }
    try {
        engine.registerHandler("", {});
    } catch (const std::invalid_argument& error) {
        std::cout << error.what() << '\n';
        ++rejected;
    }

    engine.start();
    try {
        engine.submit("UNKNOWN", "payload");
    } catch (const std::invalid_argument& error) {
        std::cout << error.what() << '\n';
        ++rejected;
    }
    const auto id = engine.submit("REPORT", "monthly-sales");
    engine.stop();

    try {
        engine.submit("REPORT", "after shutdown");
    } catch (const std::logic_error& error) {
        std::cout << error.what() << '\n';
        ++rejected;
    }
    if (rejected != 5 || engine.getStatus(id) != jobq::JobStatus::Failed ||
        engine.metrics().failedExecutions != 1) {
        return 1;
    }
    std::cout << "Observed handler failure and handled five API errors\n";
    return 0;
}
