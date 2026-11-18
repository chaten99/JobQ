#include <chrono>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>

#include "cli/InspectionCommands.h"
#include "cli/InteractiveRuntime.h"
#include "helpers/TemporaryDirectory.h"
#include "helpers/TestSupport.h"
#include "queue/JobQueue.h"
#include "storage/JobRecord.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;

std::string fieldValue(const std::string& report, const std::string& field) {
    std::istringstream lines(report);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with(field)) {
            const auto start = line.find_first_not_of(' ', field.size());
            return start == std::string::npos ? "" : line.substr(start);
        }
    }
    return "";
}

void testSessionAndHelp() {
    jobq::cli::InteractiveRuntime runtime({.workerCount = 3});
    const auto before = runtime.snapshot();
    check(before.state == jobq::cli::RuntimeState::Created && !before.engine && !before.uptime,
          "Unavailable runtime fields are explicit before Engine construction");
    std::ostringstream unavailable;
    runtime.commands().dispatch({"status", {}}, unavailable);
    runtime.commands().dispatch({"workers", {}}, unavailable);
    runtime.commands().dispatch({"queue", {}}, unavailable);
    check(fieldValue(unavailable.str(), "Engine") == "Unavailable" &&
          fieldValue(unavailable.str(), "Configured workers") == "Unavailable" &&
          fieldValue(unavailable.str(), "Waiting counts") == "Unavailable",
          "Unavailable statistics are not reported as zero");

    std::ostringstream help;
    runtime.commands().dispatch({"help", {}}, help);
    for (const auto* command : {"help [command]", "status", "workers", "queue", "exit", "quit"}) {
        check(help.str().find(command) != std::string::npos,
              std::string("Help includes registered command ") + command);
    }
    check(help.str().find("submit") == std::string::npos &&
          help.str().find("metrics") == std::string::npos,
          "Help does not advertise future commands");
    for (const auto* command : {"status", "workers", "queue", "help", "exit", "quit"}) {
        std::ostringstream topic;
        runtime.commands().dispatch({"help", {command}}, topic);
        check(topic.str().find(std::string("Usage: ") + command) != std::string::npos,
              std::string("Registry provides command-specific help for ") + command);
    }

    jobq::Engine* observed = nullptr;
    unsigned inspections = 0;
    runtime.commands().registerCommand("verify", "Verify read-only inspection.",
        [&](const auto&, auto&) {
            auto* engine = &runtime.engine();
            if (!observed) {
                observed = engine;
            }
            const auto first = engine->snapshot();
            std::ostringstream report;
            for (const auto* command : {"help", "status", "workers", "queue"}) {
                runtime.commands().dispatch({command, {}}, report);
            }
            const auto last = engine->snapshot();
            check(engine == observed && first.state == jobq::EngineState::Running &&
                  last.state == first.state && first.workerCount == 3 && last.workerCount == 3 &&
                  first.readyJobs == last.readyJobs && first.delayedJobs == last.delayedJobs &&
                  first.queueClosed == last.queueClosed && last.metrics.submittedJobs == 0 &&
                  last.metrics.startedExecutions == 0,
                  "All inspection commands preserve the same Engine and its state");
            check(fieldValue(report.str(), "Configured workers") == "3" &&
                  fieldValue(report.str(), "Ready waiting") == "0" &&
                  fieldValue(report.str(), "Active/idle workers") == "Unavailable",
                  "Commands show real configured workers, empty depth, and unavailable activity");
            ++inspections;
            return jobq::cli::CommandAction::Continue;
        });

    std::istringstream input(
        "help\nstatus\nworkers\nqueue\nverify\ninvalid-command\nstatus extra\nworkers extra\n"
        "queue --all\nhelp unknown\nhelp status extra\nhelp ''\nhelp status\nhelp workers\n"
        "help queue\nverify\nexit\n"
    );
    std::ostringstream output;
    std::ostringstream errors;
    check(runtime.run(input, output, errors) == 0 && inspections == 2,
          "Inspection and argument errors coexist within one persistent session");
    for (const auto* command : {"status", "workers", "queue", "help"}) {
        check(errors.str().find(std::string("Usage: ") + command) != std::string::npos,
              std::string("Invalid arguments provide usage for ") + command);
    }
    check(errors.str().find("Unknown help topic") != std::string::npos &&
          errors.str().find("Unknown command") != std::string::npos,
          "Unknown commands and help topics report clear errors without exiting");

    const auto stopped = runtime.snapshot();
    check(stopped.state == jobq::cli::RuntimeState::Stopped && stopped.engine &&
          stopped.engine->state == jobq::EngineState::Stopped && stopped.engine->workersStopped &&
          stopped.engine->queueClosed && stopped.uptime && stopped.uptime->count() >= 0 &&
          runtime.snapshot().uptime == stopped.uptime,
          "Stopped snapshots retain real final state and a frozen runtime uptime");
    std::ostringstream finalReport;
    runtime.commands().dispatch({"status", {}}, finalReport);
    runtime.commands().dispatch({"workers", {}}, finalReport);
    runtime.commands().dispatch({"queue", {}}, finalReport);
    check(fieldValue(finalReport.str(), "Runtime") == "Stopped" &&
          fieldValue(finalReport.str(), "Worker pool") == "Stopped" &&
          fieldValue(finalReport.str(), "Queue state") == "Closed" &&
          !runtime.engine().isRunning(), "Post-shutdown inspection cannot restart workers");
}

void testActiveQueueAndConcurrentShutdown() {
    jobq_test::Gate release;
    jobq_test::Counter entered;
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("BLOCK", [&](const jobq::Job&) {
        entered.add();
        if (!release.wait()) {
            throw std::runtime_error("Blocked handler timed out");
        }
    });
    engine.registerHandler("OK", [](const jobq::Job&) {});
    engine.registerHandler("FAIL", [](const jobq::Job&) { throw std::runtime_error("failure"); });
    engine.start();
    const auto blocked = engine.submit("BLOCK", "");
    check(entered.waitAtLeast(1), "Handler is active before queue inspection");
    const auto pending = engine.submit("OK", "");
    jobq::RetryPolicy retry;
    retry.maxRetries = 1;
    retry.initialDelay = 0ms;
    const auto failed = engine.submit("FAIL", "", {.retryPolicy = retry});

    auto reader = std::async(std::launch::async, [&] {
        for (int count = 0; count < 100; ++count) {
            const auto snapshot = engine.snapshot();
            jobq::cli::RuntimeSnapshot runtime;
            runtime.state = jobq::cli::RuntimeState::Running;
            runtime.engine = snapshot;
            std::ostringstream output;
            jobq::cli::printRuntimeStatus(runtime, output);
            jobq::cli::printWorkers(runtime, output);
            jobq::cli::printQueue(runtime, output);
            if (snapshot.state != jobq::EngineState::Running || snapshot.workerCount != 1 ||
                snapshot.readyJobs != 2 || snapshot.delayedJobs != 0 || snapshot.queueClosed ||
                fieldValue(output.str(), "Ready waiting") != "2" ||
                fieldValue(output.str(), "Completed executions") != "0") {
                return false;
            }
        }
        return true;
    });
    check(reader.wait_for(5s) == std::future_status::ready && reader.get(),
          "Repeated concurrent inspection preserves real pending counts while a handler is active");
    check(engine.getStatus(blocked) == jobq::JobStatus::Running &&
          engine.getStatus(pending) == jobq::JobStatus::Pending &&
          engine.getStatus(failed) == jobq::JobStatus::Pending,
          "Inspection leaves active and queued job statuses unchanged");
    auto shutdown = std::async(std::launch::async, [&] { engine.stop(); });
    check(jobq_test::waitUntil([&] { return engine.snapshot().state == jobq::EngineState::Stopping; }),
          "Snapshot observes Engine shutdown while its handler is blocked");
    const auto stopping = engine.snapshot();
    check(stopping.readyJobs == 2 && stopping.workerCount == 1,
          "Inspection remains available while shutdown waits for work");
    release.open();
    shutdown.get();
    const auto final = engine.snapshot();
    check(final.state == jobq::EngineState::Stopped && final.workersStopped &&
          final.queueClosed && final.readyJobs == 0 && final.delayedJobs == 0 &&
          final.metrics.completedExecutions == 2 && final.metrics.failedExecutions == 2 &&
          engine.getStatus(failed) == jobq::JobStatus::Failed,
          "Final snapshot separates two failed attempts from the single failed job");
}

void testDelayedSnapshotsAndUnavailableState() {
    jobq::JobQueue queue;
    auto job = std::make_shared<jobq::Job>(1, "TASK", "");
    queue.pushRetry(job, 0ms);
    queue.pushRetry(std::make_shared<jobq::Job>(2, "TASK", ""), 1h);
    const auto first = queue.snapshot();
    const auto second = queue.snapshot();
    check(first.readyJobs == 1 && first.delayedJobs == 1 && !first.closed &&
          first.readyJobs == second.readyJobs && first.delayedJobs == second.delayedJobs &&
          queue.tryPop() == job,
          "Queue snapshots distinguish ready and retry entries without popping them");

    jobq_test::TemporaryDirectory directory("inspection_recovery");
    jobq::JobRecord record;
    record.id = 9;
    record.type = "TASK";
    record.attemptsMade = 1;
    record.retryPolicy.maxRetries = 1;
    record.retryPolicy.initialDelay = std::chrono::duration_cast<std::chrono::milliseconds>(1h);
    record.retryPolicy.maxDelay = record.retryPolicy.initialDelay;
    {
        std::ofstream journal(directory.journal(), std::ios::binary);
        journal << jobq::serializeJobRecord(record) << '\n';
    }
    jobq::Engine engine({.workerCount = 1, .storagePath = directory.journal()});
    const auto delayed = engine.snapshot();
    jobq::cli::RuntimeSnapshot runtime;
    runtime.engine = delayed;
    runtime.uptime = 3661s;
    std::ostringstream output;
    jobq::cli::printQueue(runtime, output);
    jobq::cli::printRuntimeStatus(runtime, output);
    check(delayed.state == jobq::EngineState::Created && delayed.delayedJobs == 1 &&
          delayed.readyJobs == 0 && fieldValue(output.str(), "Retry waiting") == "1" &&
          fieldValue(output.str(), "Uptime") == "01:01:01" &&
          engine.getStatus(9) == jobq::JobStatus::Pending,
          "Real recovered retry depth is exposed and duration rendering uses hours, minutes, seconds");
    engine.stop();
    check(engine.snapshot().delayedJobs == 1 && engine.getStatus(9) == jobq::JobStatus::Pending,
          "Inspection and stopping before startup retain recovered waiting work");

    jobq::cli::InteractiveRuntime invalid({.workerCount = 0});
    std::istringstream input;
    std::ostringstream errors;
    check(invalid.run(input, output, errors) == 1 &&
          invalid.snapshot().state == jobq::cli::RuntimeState::Failed && !invalid.snapshot().engine,
          "Zero-worker startup fails and exposes unavailable Engine information");
    for (const auto state : {jobq::cli::RuntimeState::Starting, jobq::cli::RuntimeState::Stopping}) {
        jobq::cli::RuntimeSnapshot transition;
        transition.state = state;
        std::ostringstream report;
        jobq::cli::printRuntimeStatus(transition, report);
        check(fieldValue(report.str(), "Runtime") ==
              (state == jobq::cli::RuntimeState::Starting ? "Starting" : "Stopping") &&
              fieldValue(report.str(), "Uptime") == "Unavailable",
              "Transition rendering distinguishes real lifecycle labels from unavailable uptime");
    }
}

void testConcurrentRuntimeSnapshots() {
    jobq_test::Counter entered;
    jobq_test::Gate release;
    jobq::cli::InteractiveRuntime runtime({.workerCount = 1});
    runtime.commands().registerCommand("pause", "Pause the test session.",
        [&](const auto&, auto&) {
            entered.add();
            if (!release.wait()) {
                throw std::runtime_error("Session test timed out");
            }
            return jobq::cli::CommandAction::Continue;
        });
    std::istringstream input("pause\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    auto session = std::async(std::launch::async, [&] { return runtime.run(input, output, errors); });
    check(entered.waitAtLeast(1), "Session is running before concurrent snapshots");
    bool running = true;
    for (int count = 0; count < 100; ++count) {
        const auto snapshot = runtime.snapshot();
        running = running && snapshot.state == jobq::cli::RuntimeState::Running &&
                  snapshot.engine && snapshot.engine->state == jobq::EngineState::Running &&
                  snapshot.engine->workerCount == 1 && snapshot.uptime.has_value();
    }
    release.open();
    check(session.get() == 0 && running && errors.str().empty() &&
          runtime.snapshot().state == jobq::cli::RuntimeState::Stopped,
          "Runtime snapshots safely observe another session thread and its final shutdown");
}

}

int main() {
    try {
        testSessionAndHelp();
        testActiveQueueAndConcurrentShutdown();
        testDelayedSnapshotsAndUnavailableState();
        testConcurrentRuntimeSnapshots();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
