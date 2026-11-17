#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "jobq/Engine.h"
#include "../helpers/TemporaryDirectory.h"
#include "../helpers/TestSupport.h"

namespace {

using jobq_test::check;
using jobq_test::throwsWith;

void testPayloadAndHandlerIdentity() {
    const std::string binary("a\0b\0c", 5);
    std::array<std::string, 2> received;
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("TASK", [&](const jobq::Job& job) {
        received[0] = job.getPayload();
    });
    engine.registerHandler("task", [&](const jobq::Job& job) {
        received[1] = job.getPayload();
    });
    engine.start();
    auto payload = binary;
    const auto binaryId = engine.submit("TASK", std::move(payload));
    const auto emptyId = engine.submit("task", "");
    engine.stop();

    check(received[0] == binary && received[0].size() == 5 && received[1].empty(),
          "Binary and empty payloads survive submission intact");
    check(binaryId > 0 && emptyId > 0 && binaryId != emptyId,
          "Both case-sensitive handlers return valid distinct identifiers");
    check(engine.getStatus(binaryId) == jobq::JobStatus::Completed &&
          engine.getStatus(emptyId) == jobq::JobStatus::Completed,
          "Both case-sensitive handlers complete their own jobs");
}

void testWorkerConfiguration() {
    jobq_test::Counter entered;
    jobq_test::Gate release;
    std::atomic<unsigned> timedOut{0};
    jobq::Engine engine({.workerCount = 3});
    engine.registerHandler("BLOCK", [&](const jobq::Job&) {
        entered.add();
        if (!release.wait()) {
            ++timedOut;
        }
    });
    engine.start();
    for (int i = 0; i < 3; ++i) {
        engine.submit("BLOCK", "");
    }
    const auto concurrent = entered.waitAtLeast(3);
    release.open();
    engine.stop();
    check(concurrent && timedOut == 0,
          "Custom worker count runs three handlers concurrently");
    check(engine.metrics().completedExecutions == 3,
          "Blocked handlers drain after deterministic release");
}

void testRetryExhaustionAndWorkerSurvival() {
    std::atomic<unsigned> attempts{0};
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("FAIL", [&](const jobq::Job&) {
        ++attempts;
        throw 42;
    });
    engine.registerHandler("OK", [](const jobq::Job&) {});
    engine.start();
    jobq::RetryPolicy policy;
    policy.maxRetries = 2;
    policy.initialDelay = std::chrono::milliseconds::zero();
    const auto failed = engine.submit("FAIL", "", {.retryPolicy = policy});
    const auto healthy = engine.submit("OK", "");
    engine.stop();
    const auto metrics = engine.metrics();
    check(attempts == 3 && engine.getStatus(failed) == jobq::JobStatus::Failed,
          "Non-standard exceptions exhaust the configured retry budget");
    check(engine.getStatus(healthy) == jobq::JobStatus::Completed &&
          metrics.completedExecutions == 1 && metrics.failedExecutions == 3 &&
          metrics.startedExecutions == 4 && metrics.scheduledRetries == 2,
          "Worker survives non-standard exceptions with exact retry accounting");
}

void testInvalidOptionsDoNotSubmit() {
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("TASK", [](const jobq::Job&) {});
    engine.start();
    for (const auto multiplier : {0.5, std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()}) {
        jobq::RetryPolicy policy;
        policy.multiplier = multiplier;
        check(throwsWith<std::invalid_argument>([&] {
            engine.submit("TASK", "invalid", {.retryPolicy = policy});
        }, "multiplier"), "Invalid retry multiplier propagates an argument error");
    }
    jobq::RetryPolicy policy;
    policy.maxDelay = std::chrono::milliseconds(-1);
    check(throwsWith<std::invalid_argument>([&] {
        engine.submit("TASK", "invalid", {.retryPolicy = policy});
    }, "negative"), "Negative maximum retry delay is rejected");
    check(engine.metrics().submittedJobs == 0,
          "Invalid options do not count as accepted submissions");
    const auto valid = engine.submit("TASK", "valid");
    engine.stop();
    check(engine.getStatus(valid) == jobq::JobStatus::Completed &&
          engine.metrics().submittedJobs == 1,
          "Engine remains usable after rejected options");
}

void testStorageSubmissionFailure() {
    jobq_test::TemporaryDirectory directory("api_submission_error");
    jobq::Engine engine({.workerCount = 1, .storagePath = directory.journal()});
    engine.registerHandler("TASK", [](const jobq::Job&) {});
    engine.start();
    std::filesystem::create_directory(directory.journal());
    check(throwsWith<std::runtime_error>([&] {
        engine.submit("TASK", "cannot persist");
    }, ""), "Journal write failure propagates through public submission");
    check(engine.metrics().submittedJobs == 0 && !engine.getStatus(1),
          "Failed persisted submission leaves no registered or accepted job");
    std::filesystem::remove(directory.journal());
    const auto id = engine.submit("TASK", "persisted");
    engine.stop();
    check(engine.getStatus(id) == jobq::JobStatus::Completed &&
          engine.metrics().completedExecutions == 1,
          "Submission succeeds after the external storage problem is corrected");
}

}

int main() {
    try {
        testPayloadAndHandlerIdentity();
        testWorkerConfiguration();
        testRetryExhaustionAndWorkerSurvival();
        testInvalidOptionsDoNotSubmit();
        testStorageSubmissionFailure();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::cout << "Tests run: " << jobq_test::testsRun()
              << ", failed: " << jobq_test::testsFailed() << '\n';
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
