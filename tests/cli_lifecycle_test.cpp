#include <csignal>
#include <future>
#include <sstream>
#include <stdexcept>

#include "cli/InteractiveRuntime.h"
#include "cli/TerminalSession.h"
#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;
volatile std::sig_atomic_t restoredSignal = 0;
extern "C" void previousHandler(int signal) { restoredSignal = signal; }

void testDrainAndRequests() {
    jobq_test::Counter entered;
    jobq_test::Gate release;
    jobq_test::Gate command;
    jobq::cli::InteractiveRuntime runtime({.workerCount = 2}, [&](jobq::Engine& engine) {
        engine.registerHandler("BLOCK", [&](const jobq::Job&) {
            entered.add();
            if (!release.wait()) throw std::runtime_error("Handler timeout");
        });
        engine.registerHandler("OK", [](const jobq::Job&) {});
    });
    runtime.commands().registerCommand("work", "Start test work.", [&](const auto&, auto&) {
        runtime.submitJob("BLOCK", "one");
        runtime.submitJob("BLOCK", "two");
        if (!entered.waitAtLeast(2)) throw std::runtime_error("Workers did not start");
        runtime.submitJob("OK", "queued one");
        runtime.submitJob("OK", "queued two");
        command.open();
        if (!runtime.waitForShutdown(5s)) throw std::runtime_error("Shutdown not requested");
        return jobq::cli::CommandAction::Continue;
    });
    std::istringstream input("work\nsubmit OK rejected\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    auto session = std::async(std::launch::async, [&] { return runtime.run(input, output, errors); });
    check(command.wait(), "Two active and two waiting jobs are accepted before shutdown");
    auto first = std::async(std::launch::async, [&] { runtime.requestShutdown(); });
    auto second = std::async(std::launch::async, [&] { runtime.requestShutdown(); });
    first.get();
    second.get();
    check(jobq_test::waitUntil([&] { return runtime.snapshot().state == jobq::cli::RuntimeState::Stopping; }),
          "One coordinator enters Stopping while workers are still active");
    check(jobq_test::throwsWith<std::logic_error>([&] { runtime.submitJob("OK", "late"); }, "running") &&
          session.wait_for(0ms) == std::future_status::timeout,
          "Runtime rejects late submissions and waits for active handlers rather than destroying them");
    release.open();
    check(session.wait_for(5s) == std::future_status::ready && session.get() == 0 && errors.str().empty(),
          "Concurrent requests converge on a successful joined shutdown");
    const auto jobs = runtime.engine().listJobs();
    bool complete = jobs.size() == 4;
    for (const auto& job : jobs) complete = complete && job.status == jobq::JobStatus::Completed;
    const auto metrics = runtime.engine().metrics();
    check(complete && metrics.submittedJobs == 4 && metrics.completedExecutions == 4 &&
          metrics.startedExecutions == 4 && metrics.failedExecutions == 0 &&
          runtime.snapshot().state == jobq::cli::RuntimeState::Stopped,
          "All accepted active/queued work drains and metrics remain consistent");
    const auto marker = output.str().find("Shutting down JobQ...");
    check(marker != std::string::npos && output.str().find("Shutting down JobQ...", marker + 1) == std::string::npos,
          "Only one runtime shutdown sequence is reported");
    runtime.requestShutdown();
    check(runtime.snapshot().state == jobq::cli::RuntimeState::Stopped,
          "Requests after shutdown preserve the final state");
}

void testStartupAndFailures() {
    jobq::cli::InteractiveRuntime partial({.workerCount = 1}, [](jobq::Engine& engine) {
        engine.start();
        throw std::runtime_error("initializer failed after acquiring workers");
    });
    std::istringstream input("exit\n");
    std::ostringstream output;
    std::ostringstream errors;
    check(partial.run(input, output, errors) == 1 && !partial.engine().isRunning() &&
          partial.engine().snapshot().workersStopped && partial.snapshot().state == jobq::cli::RuntimeState::Failed,
          "Partial initialization failure closes and joins acquired workers before returning Failed");
    jobq::cli::InteractiveRuntime startup({}, [](jobq::Engine& engine) { engine.start(); });
    std::istringstream startupInput("exit\n");
    check(startup.run(startupInput, output, errors) == 1 && startup.engine().snapshot().workersStopped &&
          startup.snapshot().state == jobq::cli::RuntimeState::Failed,
          "A failed repeated Engine startup cleans up the previously acquired worker pool");
    jobq::cli::InteractiveRuntime cancelled;
    cancelled.requestShutdown();
    check(cancelled.snapshot().state == jobq::cli::RuntimeState::ShutdownRequested,
          "A request has an explicit observable ShutdownRequested state before coordination");
    std::istringstream unread("submit unknown value\n");
    std::ostringstream cancelledOutput;
    check(cancelled.run(unread, cancelledOutput, errors) == 0 &&
          cancelledOutput.str().find("Runtime started.") == std::string::npos &&
          cancelled.snapshot().state == jobq::cli::RuntimeState::Stopped,
          "A pre-start request prevents workers and command execution from starting");
    jobq::cli::InteractiveRuntime stopped;
    stopped.commands().registerCommand("fail-shutdown-output", "Fail test output.", [&](const auto&, auto& stream) {
        stream.setstate(std::ios::badbit);
        stopped.requestShutdown();
        return jobq::cli::CommandAction::Continue;
    });
    std::istringstream commands("fail-shutdown-output\n");
    std::ostringstream broken;
    check(stopped.run(commands, broken, errors) == 1 && !stopped.engine().isRunning() &&
          stopped.snapshot().state == jobq::cli::RuntimeState::Failed,
          "Shutdown output failure is recorded separately while Engine cleanup still completes");
}

void testMonitorCancellationAndSignals() {
    jobq_test::Gate waiting;
    jobq::cli::InteractiveRuntime runtime;
    runtime.commands().registerCommand("long-monitor", "Monitor with cancellation readiness.",
        [&](const auto&, auto& output) {
            waiting.open();
            return runtime.commands().dispatch({"monitor", {"10000", "6"}}, output);
        });
    std::istringstream input("long-monitor\nstatus\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    auto session = std::async(std::launch::async, [&] { return runtime.run(input, output, errors); });
    check(waiting.wait(), "Monitoring cancellation test reaches the command");
    runtime.requestShutdown(SIGINT);
    check(session.wait_for(2s) == std::future_status::ready && session.get() == 130 &&
          output.str().find("Monitoring completed.") == std::string::npos &&
          output.str().find("JobQ Runtime Status") == std::string::npos,
          "A ten-second monitoring wait is interrupted promptly and no later command starts");
    const auto original = std::signal(SIGTERM, previousHandler);
    {
        jobq::cli::TerminalSession signals;
        jobq::cli::InteractiveRuntime conflict;
        std::ostringstream setupOutput;
        std::ostringstream setupErrors;
        check(conflict.runTerminal(setupOutput, setupErrors) == 1 &&
              conflict.snapshot().state == jobq::cli::RuntimeState::Failed &&
              setupErrors.str().find("one terminal") != std::string::npos,
              "Signal-session registration conflict fails before Engine initialization");
        std::raise(SIGTERM);
        check(signals.signalNumber() == SIGTERM, "The C termination signal only records a notification");
        signals.restore();
    }
    std::raise(SIGTERM);
    check(restoredSignal == SIGTERM, "Previous host signal disposition is restored after the session");
    std::signal(SIGTERM, original);
}

}

int main() {
    try {
        testDrainAndRequests();
        testStartupAndFailures();
        testMonitorCancellationAndSignals();
    } catch (const std::exception& error) { check(false, error.what()); }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
