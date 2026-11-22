#include <cmath>
#include <future>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "cli/InteractiveRuntime.h"
#include "cli/MetricsCommands.h"
#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;

std::size_t countText(const std::string& text, const std::string& token) {
    std::size_t count = 0;
    for (auto position = text.find(token); position != std::string::npos;
         position = text.find(token, position + token.size())) ++count;
    return count;
}

std::size_t countSamples(const std::string& text) {
    const std::regex heading("\nSample [0-9]+\n");
    return static_cast<std::size_t>(std::distance(
        std::sregex_iterator(text.begin(), text.end(), heading), std::sregex_iterator()));
}

void testOptionsAndIntervals() {
    const auto defaults = jobq::cli::parseMonitorOptions({"monitor", {}});
    check(defaults.interval == 1000ms && defaults.sampleCount == 5,
          "Default monitoring uses five one-second intervals");
    const auto custom = jobq::cli::parseMonitorOptions({"monitor", {"100", "3"}});
    check(custom.interval == 100ms && custom.sampleCount == 3, "Custom interval and sample count are parsed together");
    for (const auto& arguments : {
        std::vector<std::string>{"1"}, {"1", "2", "3"}, {"0", "1"}, {"-1", "1"}, {"+1", "1"},
        {"1.5", "1"}, {"abc", "1"}, {"", "1"}, {"1", "0"}, {"1", "-1"}, {"1", "61"},
        {"10001", "1"}, {"10000", "7"}, {"18446744073709551616", "1"},
        {"1", "18446744073709551615"}, {"1x", "2"}, {" 1", "2"}, {"1", "abc"}, {"1", ""}, {"1", "1.5"}
    }) {
        check(jobq_test::throwsWith<std::invalid_argument>([&] {
            jobq::cli::parseMonitorOptions({"monitor", arguments});
        }, "Usage: monitor"), "Malformed/out-of-bound arguments are rejected before sampling");
    }
    check(jobq::cli::parseMonitorOptions({"monitor", {"10000", "6"}}).sampleCount == 6 &&
          jobq::cli::parseMonitorOptions({"monitor", {"1000", "60"}}).sampleCount == 60,
          "Inclusive wait-budget and sample-count boundaries are accepted");
    jobq::JobMetricsSnapshot first;
    first.submittedJobs = 10;
    first.startedExecutions = 12;
    first.completedExecutions = 7;
    first.failedExecutions = 2;
    first.scheduledRetries = 2;
    first.finishedExecutions = 9;
    auto second = first;
    second.submittedJobs += 4;
    second.startedExecutions += 6;
    second.completedExecutions += 3;
    second.failedExecutions += 2;
    second.scheduledRetries += 1;
    second.finishedExecutions += 5;
    const auto delta = jobq::cli::calculateMetricsInterval(first, second, 2500ms);
    check(delta && delta->submittedJobs == 4 && delta->startedExecutions == 6 && delta->completedExecutions == 3 &&
          delta->failedExecutions == 2 && delta->scheduledRetries == 1 && delta->finishedExecutions == 5 &&
          std::abs(delta->finishedExecutionsPerSecond - 2.0) < 1e-9,
          "Every delta and finished-attempt rate uses actual elapsed seconds");
    for (auto member : {&jobq::JobMetricsSnapshot::submittedJobs, &jobq::JobMetricsSnapshot::startedExecutions,
                       &jobq::JobMetricsSnapshot::completedExecutions, &jobq::JobMetricsSnapshot::failedExecutions,
                       &jobq::JobMetricsSnapshot::scheduledRetries, &jobq::JobMetricsSnapshot::finishedExecutions}) {
        auto reset = second;
        reset.*member = first.*member - 1;
        check(!jobq::cli::calculateMetricsInterval(first, reset, 1s),
              "Counter decrease makes interval statistics unavailable instead of wrapping");
    }
    check(!jobq::cli::calculateMetricsInterval(first, second, 0ns) &&
          !jobq::cli::calculateMetricsInterval(first, second, -1ns), "Nonpositive elapsed time never divides by zero");
    first.elapsedTime = 1s;
    check(!jobq::cli::calculateMetricsInterval(first, second, 1s), "Decreased collector elapsed time detects a reset");
    first.elapsedTime = 0ns;
    first.totalExecutionTime = 1s;
    check(!jobq::cli::calculateMetricsInterval(first, second, 1s), "Decreased accumulated duration detects a reset");
    const auto resumed = jobq::cli::calculateMetricsInterval({}, {}, 1s);
    check(resumed && resumed->finishedExecutions == 0 && resumed->finishedExecutionsPerSecond == 0,
          "A subsequent consistent baseline resumes zero-rate reporting");
    auto inconsistent = second;
    ++inconsistent.finishedExecutions;
    check(!jobq::cli::calculateMetricsInterval({}, inconsistent, 1s),
          "Inconsistent finished/completed/failed totals make interval statistics unavailable");
    jobq::JobMetricsSnapshot large;
    large.submittedJobs = std::numeric_limits<std::uint64_t>::max();
    auto near = large;
    --near.submittedJobs;
    check(jobq::cli::calculateMetricsInterval(near, large, 1s)->submittedJobs == 1,
          "Large unsigned counters produce bounded differences without signed narrowing");
}

void testInjectedTimingAndWorkers() {
    jobq_test::Gate release;
    jobq_test::Counter entered;
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("TASK", [&](const jobq::Job&) {
        entered.add();
        if (!release.wait()) throw std::runtime_error("Handler timed out");
    });
    engine.start();
    engine.submit("TASK", "");
    check(entered.waitAtLeast(1), "Monitoring begins while a real job is active");
    std::chrono::steady_clock::time_point now{};
    unsigned waits = 0;
    jobq::cli::MonitorTiming timing;
    timing.now = [&] { return now; };
    timing.wait = [&](std::chrono::milliseconds requested) {
        check(requested == 100ms, "Configured wait interval reaches the wait abstraction");
        ++waits;
        now += 250ms;
        if (waits == 1) {
            release.open();
            check(jobq_test::waitUntil([&] { return engine.metrics().finishedExecutions == 1; }),
                  "Workers update metrics during the wait without Engine/metrics locks being held");
        }
    };
    std::ostringstream output;
    jobq::cli::monitorMetrics(engine, {100ms, 3}, output, timing);
    check(waits == 3 && countSamples(output.str()) == 3 && countText(output.str(), "Baseline") == 1 &&
          output.str().find("250.000 ms") != std::string::npos &&
          output.str().find("4.000 executions/s") != std::string::npos &&
          output.str().find("Monitoring completed.") != std::string::npos,
          "Three samples follow a baseline and measure 250 ms rather than assuming the requested 100 ms");
    check(engine.metrics().submittedJobs == 1 && engine.metrics().finishedExecutions == 1 && engine.isRunning(),
          "Monitoring neither changes counters nor stops the Engine");
    waits = 0;
    jobq::cli::MonitorTiming defaultsTiming{
        [&] { return now; }, [&](std::chrono::milliseconds requested) {
            check(requested == 1000ms, "Default monitoring requests a one-second wait");
            ++waits;
            now += requested;
        }
    };
    std::ostringstream defaults;
    jobq::cli::monitorMetrics(engine, {}, defaults, defaultsTiming);
    check(waits == 5 && countSamples(defaults.str()) == 5,
          "Default sampling is bounded without real sleeps in the unit test");
    waits = 0;
    std::ostringstream reset;
    jobq::cli::MonitorTiming resetTiming{
        [&] { return now; }, [&](std::chrono::milliseconds) {
            now += ++waits == 1 ? -1ms : 2ms;
        }
    };
    jobq::cli::monitorMetrics(engine, {1ms, 2}, reset, resetTiming);
    check(countText(reset.str(), "Unavailable:") == 1 && countSamples(reset.str()) == 2 &&
          reset.str().find("Monitoring completed.") != std::string::npos,
          "An invalid interval is reported and monitoring continues from the new baseline");
    std::ostringstream invalid;
    check(jobq_test::throwsWith<std::invalid_argument>([&] {
        jobq::cli::monitorMetrics(engine, {0ms, 5}, invalid, timing);
    }, "Usage: monitor") && invalid.str().empty(), "Invalid options are rejected before writing a baseline");
    std::ostringstream bad;
    bad.setstate(std::ios::badbit);
    check(jobq_test::throwsWith<std::runtime_error>([&] {
        jobq::cli::monitorMetrics(engine, {1ms, 1}, bad, timing);
    }, "output"), "Failed output aborts monitoring without reporting completion");
    engine.stop();
    check(jobq_test::throwsWith<std::logic_error>([&] {
        jobq::cli::monitorMetrics(engine, {1ms, 1}, output);
    }, "running"), "Monitoring cannot restart a stopped Engine");
}

void testShutdownAndSession() {
    jobq::Engine engine({.workerCount = 1});
    engine.start();
    std::chrono::steady_clock::time_point now{};
    jobq::cli::MonitorTiming stopping{
        [&] { return now; }, [&](std::chrono::milliseconds interval) {
            auto shutdown = std::async(std::launch::async, [&] { engine.stop(); });
            shutdown.get();
            now += interval;
        }
    };
    std::ostringstream stopped;
    jobq::cli::monitorMetrics(engine, {1ms, 3}, stopped, stopping);
    check(stopped.str().find("Monitoring stopped: Engine is not running.") != std::string::npos &&
          stopped.str().find("Monitoring completed.") == std::string::npos &&
          countSamples(stopped.str()) == 0,
          "Concurrent shutdown during a wait ends monitoring before another sample");
    jobq::Engine* seen = nullptr;
    jobq::cli::InteractiveRuntime runtime({.workerCount = 1}, [&](jobq::Engine& current) {
        seen = &current;
        current.registerHandler("TASK", [](const jobq::Job&) {});
    });
    runtime.commands().registerCommand("start-job", "Submit actual session work.",
        [&](const auto&, auto&) {
            const auto id = runtime.engine().submit("TASK", "session metrics");
            check(jobq_test::waitUntil([&] { return runtime.engine().metrics().finishedExecutions == 1; }) &&
                  runtime.engine().getStatus(id) == jobq::JobStatus::Completed,
                  "Session metrics reflect actual work processed by the persistent Engine");
            return jobq::cli::CommandAction::Continue;
        });
    runtime.commands().registerCommand("verify-engine", "Verify persistent Engine.",
        [&](const auto&, auto&) {
            check(&runtime.engine() == seen && runtime.engine().isRunning(),
                  "Commands after monitoring still use the same running Engine");
            return jobq::cli::CommandAction::Continue;
        });
    std::istringstream input("help\nhelp monitor\nstart-job\nmetrics\nmonitor 0 3\nmonitor 1\nmonitor 100 3\n"
                             "metrics\nstatus\nverify-engine\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    const auto result = runtime.run(input, output, errors);
    check(result == 0, "The monitored session exits successfully");
    check(countSamples(output.str()) == 3, "The interactive monitor prints exactly three samples");
    check(errors.str().find("Usage: monitor") != std::string::npos, "Invalid monitor arguments display usage");
    check(output.str().find("JobQ Runtime Status") != std::string::npos, "Status executes after monitoring");
    check(output.str().find("Runtime stopped.") != std::string::npos, "The session reports completed shutdown");
    check(output.str().find("Submitted jobs              1") != std::string::npos &&
          output.str().find("Finished executions         1") != std::string::npos &&
          runtime.engine().metrics().submittedJobs == 1 && runtime.engine().metrics().finishedExecutions == 1,
          "Registered metrics and monitoring commands show actual session counters without altering them");
}

}

int main() {
    try {
        testOptionsAndIntervals();
        testInjectedTimingAndWorkers();
        testShutdownAndSession();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
