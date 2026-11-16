#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "jobq/Engine.h"
#include "helpers/TemporaryDirectory.h"
#include "helpers/TestSupport.h"
#include "storage/JobRecord.h"

namespace {

using namespace std::chrono_literals;
using jobq_test::check;
using jobq_test::throwsWith;

void testConstructionAndLifecycle() {
    static_assert(!std::is_copy_constructible_v<jobq::Engine>);
    static_assert(!std::is_move_constructible_v<jobq::Engine>);

    check(throwsWith<std::invalid_argument>([] {
        jobq::Engine engine({.workerCount = 0});
    }, "Worker count"), "Zero workers are rejected");

    jobq::Engine engine;
    check(!engine.isRunning(), "New engine has no running workers");
    check(!engine.getStatus(123), "Unknown job ID has no status");
    check(engine.metrics().submittedJobs == 0, "New metrics are empty");
    check(throwsWith<std::logic_error>([&] {
        engine.submit("TASK", "before start");
    }, "running engine"), "Submission before start is rejected");

    engine.registerHandler("TASK", [](const jobq::Job&) {});
    engine.start();
    check(engine.isRunning(), "Start runs the engine");
    check(throwsWith<std::logic_error>([&] { engine.start(); }, "only be started once"),
          "Repeated start is rejected");
    check(throwsWith<std::logic_error>([&] {
        engine.registerHandler("LATE", [](const jobq::Job&) {});
    }, "before start"), "Registration after start is rejected");

    const auto id = engine.submit("TASK", "payload");
    engine.stop();
    engine.stop();
    check(!engine.isRunning(), "Repeated stop is harmless");
    check(engine.getStatus(id) == jobq::JobStatus::Completed,
          "Stop drains the accepted job");
    check(throwsWith<std::logic_error>([&] {
        engine.submit("TASK", "after stop");
    }, "running engine"), "Submission after stop is rejected");
    check(throwsWith<std::logic_error>([&] { engine.start(); }, "only be started once"),
          "Stopped engines cannot restart");

    jobq::Engine unused;
    unused.stop();
    unused.stop();
    check(throwsWith<std::logic_error>([&] { unused.start(); }, "only be started once"),
          "Stopping before start closes the engine");
}

void testRegistrationAndPayloads() {
    jobq::Engine engine({.workerCount = 2});
    check(throwsWith<std::invalid_argument>([&] {
        engine.registerHandler("", [](const jobq::Job&) {});
    }, "non-empty"), "Empty handler types are rejected");
    check(throwsWith<std::invalid_argument>([&] {
        engine.registerHandler("EMPTY", {});
    }, "non-empty"), "Empty handlers are rejected");

    std::atomic<unsigned> mailCalls{0};
    std::atomic<unsigned> reportCalls{0};
    std::atomic<bool> payloadMatches{false};
    const std::string expected(100000, 'x');
    engine.registerHandler("MAIL", [&](const jobq::Job& job) {
        payloadMatches = job.getPayload() == expected;
        ++mailCalls;
    });
    engine.registerHandler("REPORT", [&](const jobq::Job&) { ++reportCalls; });
    check(throwsWith<std::invalid_argument>([&] {
        engine.registerHandler("MAIL", [&](const jobq::Job&) { ++reportCalls; });
    }, "already registered"), "Duplicate registration is rejected");

    engine.start();
    check(throwsWith<std::invalid_argument>([&] {
        engine.submit("UNKNOWN", "payload");
    }, "No handler"), "Unknown job types fail before submission");
    auto payload = expected;
    const auto mailId = engine.submit("MAIL", std::move(payload));
    const auto reportId = engine.submit("REPORT", std::string("a\0b", 3));
    engine.stop();

    check(mailCalls == 1 && reportCalls == 1,
          "Both original handlers execute exactly once");
    check(payloadMatches, "Moved large payload reaches the handler intact");
    check(mailId != reportId, "Submissions return distinct job IDs");
    check(engine.getStatus(mailId) == jobq::JobStatus::Completed &&
          engine.getStatus(reportId) == jobq::JobStatus::Completed,
          "Both job statuses are available after shutdown");
    check(engine.metrics().submittedJobs == 2 &&
          engine.metrics().completedExecutions == 2,
          "Rejected unknown jobs do not affect metrics");
}

void testPriorityAndRetries() {
    jobq::Engine engine({.workerCount = 1});
    std::latch blockerStarted{1};
    std::latch releaseBlocker{1};
    std::vector<std::string> order;
    std::atomic<unsigned> attempts{0};

    engine.registerHandler("BLOCK", [&](const jobq::Job&) {
        blockerStarted.count_down();
        releaseBlocker.wait();
    });
    engine.registerHandler("ORDER", [&](const jobq::Job& job) {
        order.push_back(job.getPayload());
    });
    engine.registerHandler("RETRY", [&](const jobq::Job& job) {
        ++attempts;
        if (job.getAttemptsMade() == 1) {
            throw std::runtime_error("retry once");
        }
    });
    engine.registerHandler("FAIL", [](const jobq::Job&) {
        throw std::runtime_error("permanent failure");
    });

    engine.start();
    engine.submit("BLOCK", "");
    blockerStarted.wait();
    engine.submit("ORDER", "low", {.priority = jobq::JobPriority::Low});
    engine.submit("ORDER", "high-first", {.priority = jobq::JobPriority::High});
    engine.submit("ORDER", "high-second", {.priority = jobq::JobPriority::High});
    engine.submit("ORDER", "normal");

    jobq::RetryPolicy retry;
    retry.maxRetries = 1;
    retry.initialDelay = 1ms;
    retry.maxDelay = 1ms;
    const auto retryId = engine.submit("RETRY", "", {.retryPolicy = retry});
    const auto failedId = engine.submit("FAIL", "");
    check(throwsWith<std::invalid_argument>([&] {
        engine.submit("ORDER", "invalid", {
            .priority = static_cast<jobq::JobPriority>(99)
        });
    }, "Invalid initial"), "Invalid priority is rejected by the core");
    retry.initialDelay = -1ms;
    check(throwsWith<std::invalid_argument>([&] {
        engine.submit("RETRY", "", {.retryPolicy = retry});
    }, "negative"), "Invalid retry policy is rejected by the core");

    releaseBlocker.count_down();
    engine.stop();
    check(order == std::vector<std::string>{"high-first", "high-second", "normal", "low"},
          "Priority options preserve FIFO ordering for equal priorities");
    check(attempts == 2 && engine.getStatus(retryId) == jobq::JobStatus::Completed,
          "Configured delayed retry succeeds during shutdown drain");
    check(engine.getStatus(failedId) == jobq::JobStatus::Failed,
          "Handler exceptions produce a failed job status");
    const auto metrics = engine.metrics();
    check(metrics.submittedJobs == 7 && metrics.startedExecutions == 8 &&
          metrics.completedExecutions == 6 && metrics.failedExecutions == 2 &&
          metrics.scheduledRetries == 1,
          "Retries and failures preserve exact core metrics");
}

void testOwnershipAndDestructor() {
    std::atomic<unsigned> executions{0};
    auto owned = std::make_shared<unsigned>(7);
    std::weak_ptr<unsigned> lifetime = owned;
    {
        jobq::Engine engine({.workerCount = 2});
        jobq::JobHandler handler = [owned, &executions](const jobq::Job&) {
            if (*owned == 7) {
                ++executions;
            }
        };
        engine.registerHandler("OWNED", handler);
        handler = {};
        owned.reset();
        check(!lifetime.expired(), "Engine owns the registered callable");
        engine.start();
        for (int i = 0; i < 40; ++i) {
            engine.submit("OWNED", "payload");
        }
    }
    check(executions == 40, "Destructor drains work and joins workers");
    check(lifetime.expired(), "Destruction releases handler captures");
}

void testHandlerReentrancyAndConcurrentStop() {
    jobq::Engine engine({.workerCount = 2});
    std::atomic<unsigned> stopRejections{0};
    std::promise<jobq::JobId> childSubmitted;
    std::latch handlerEntered{1};
    std::latch releaseHandler{1};
    engine.registerHandler("CHILD", [](const jobq::Job&) {});
    engine.registerHandler("PARENT", [&](const jobq::Job&) {
        try {
            engine.stop();
        } catch (const std::logic_error&) {
            ++stopRejections;
        }
        childSubmitted.set_value(engine.submit("CHILD", "nested"));
        handlerEntered.count_down();
        releaseHandler.wait();
        static_cast<void>(engine.metrics());
        static_cast<void>(engine.isRunning());
    });

    engine.start();
    engine.submit("PARENT", "payload");
    handlerEntered.wait();
    const auto childId = childSubmitted.get_future().get();
    std::latch stopping{2};
    auto stop = [&] {
        stopping.count_down();
        engine.stop();
    };
    auto first = std::async(std::launch::async, stop);
    auto second = std::async(std::launch::async, stop);
    stopping.wait();
    releaseHandler.count_down();
    first.get();
    second.get();

    check(stopRejections == 1, "Handlers cannot join their own engine workers");
    check(engine.getStatus(childId) == jobq::JobStatus::Completed,
          "Handlers can submit work while the engine runs");
    check(engine.metrics().completedExecutions == 2 && !engine.isRunning(),
          "Concurrent stop calls join safely while handlers inspect the engine");
}

void testConcurrentSubmissionsAndShutdown() {
    jobq::Engine engine({.workerCount = 4});
    std::atomic<unsigned> executions{0};
    engine.registerHandler("TASK", [&](const jobq::Job&) { ++executions; });
    engine.start();

    std::vector<jobq::JobId> ids;
    std::mutex idsMutex;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&] {
            std::vector<jobq::JobId> submitted;
            for (int i = 0; i < 50; ++i) {
                submitted.push_back(engine.submit("TASK", "payload"));
            }
            std::lock_guard lock(idsMutex);
            ids.insert(ids.end(), submitted.begin(), submitted.end());
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    engine.stop();
    check(std::set<jobq::JobId>(ids.begin(), ids.end()).size() == 200 && executions == 200,
          "Concurrent submissions have unique IDs and execute exactly once");

    jobq::Engine racing({.workerCount = 2});
    racing.registerHandler("TASK", [](const jobq::Job&) {});
    racing.start();
    std::latch race{2};
    std::vector<jobq::JobId> accepted;
    std::thread submitter([&] {
        race.arrive_and_wait();
        for (int i = 0; i < 100; ++i) {
            try {
                accepted.push_back(racing.submit("TASK", "payload"));
            } catch (const std::logic_error&) {
                break;
            }
        }
    });
    race.arrive_and_wait();
    racing.stop();
    submitter.join();
    check(std::all_of(accepted.begin(), accepted.end(), [&](jobq::JobId id) {
        return racing.getStatus(id) == jobq::JobStatus::Completed;
    }), "Every submission accepted during concurrent shutdown is drained");
    check(racing.metrics().submittedJobs == accepted.size(),
          "Shutdown race rejects further work without corrupting accounting");
}

void testPersistenceConfiguration() {
    jobq_test::TemporaryDirectory directory("public_api");
    jobq::JobId id{};
    {
        jobq::Engine engine({.workerCount = 1, .storagePath = directory.journal()});
        engine.registerHandler("TASK", [](const jobq::Job&) {});
        engine.start();
        id = engine.submit("TASK", "persisted");
        engine.stop();
    }
    {
        jobq::Engine recovered({.workerCount = 1, .storagePath = directory.journal()});
        check(recovered.getStatus(id) == jobq::JobStatus::Completed,
              "Storage configuration restores completed job status");
        recovered.registerHandler("TASK", [](const jobq::Job&) {});
        recovered.start();
        const auto nextId = recovered.submit("TASK", "next");
        recovered.stop();
        check(nextId > id && recovered.getStatus(nextId) == jobq::JobStatus::Completed,
              "Recovered engine continues unique job IDs");
    }
    {
        jobq::JobRecord pending;
        pending.id = 17;
        pending.type = "TASK";
        pending.payload = "recovered";
        auto unknown = pending;
        unknown.id = 21;
        unknown.type = "UNKNOWN";
        std::ofstream journal(directory.journal(), std::ios::binary | std::ios::trunc);
        journal << jobq::serializeJobRecord(pending) << '\n'
                << jobq::serializeJobRecord(unknown) << '\n';
    }
    {
        std::atomic<unsigned> calls{0};
        jobq::Engine recovered({.workerCount = 1, .storagePath = directory.journal()});
        recovered.registerHandler("TASK", [&](const jobq::Job&) { ++calls; });
        check(recovered.getStatus(17) == jobq::JobStatus::Pending,
              "Pending recovery work waits for engine startup");
        recovered.start();
        const auto nextId = recovered.submit("TASK", "new");
        recovered.stop();
        check(calls == 2 && nextId > 21 &&
              recovered.getStatus(17) == jobq::JobStatus::Completed,
              "Registered handlers process pending recovery work");
        check(recovered.getStatus(21) == jobq::JobStatus::Failed &&
              recovered.metrics().failedExecutions == 1,
              "Recovered unknown types follow core execution failure semantics");
    }
    {
        std::ofstream invalid(directory.journal(), std::ios::trunc);
        invalid << "invalid journal\n";
    }
    check(throwsWith<std::runtime_error>([&] {
        jobq::Engine invalid({.storagePath = directory.journal()});
    }, "Invalid job storage record"), "Recovery errors propagate from construction");
}

void testConcurrentStartAndStop() {
    bool stopped = true;
    for (int run = 0; run < 12; ++run) {
        jobq::Engine engine({.workerCount = 2});
        std::latch race{2};
        auto starter = std::async(std::launch::async, [&] {
            race.arrive_and_wait();
            try {
                engine.start();
            } catch (const std::logic_error&) {
            }
        });
        race.arrive_and_wait();
        engine.stop();
        starter.get();
        stopped = !engine.isRunning() && stopped;
    }
    check(stopped, "Concurrent startup and shutdown leave workers stopped");
}

}

int main() {
    try {
        testConstructionAndLifecycle();
        testRegistrationAndPayloads();
        testPriorityAndRetries();
        testOwnershipAndDestructor();
        testHandlerReentrancyAndConcurrentStop();
        testConcurrentSubmissionsAndShutdown();
        testPersistenceConfiguration();
        testConcurrentStartAndStop();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::cout << "Tests run: " << jobq_test::testsRun()
              << ", failed: " << jobq_test::testsFailed() << '\n';
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
