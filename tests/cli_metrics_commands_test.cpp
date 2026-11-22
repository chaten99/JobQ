#include <future>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "cli/InteractiveRuntime.h"
#include "cli/MetricsCommands.h"
#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;

std::string value(const std::string& report, const std::string& field) {
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

void testFormatting() {
    jobq::JobMetrics metrics;
    std::ostringstream empty;
    jobq::cli::printInteractiveMetrics(metrics.snapshot(), empty);
    check(value(empty.str(), "Submitted jobs") == "0" && value(empty.str(), "Started executions") == "0" &&
          value(empty.str(), "Finished executions") == "0" && value(empty.str(), "Average execution") == "0 ns" &&
          value(empty.str(), "Total execution") == "0 ns" &&
          value(empty.str(), "Cumulative throughput") == "0.000 executions/s",
          "A genuine empty collector reports zeros with explicit duration/rate units");
    metrics.recordJobSubmitted();
    metrics.recordExecutionStarted();
    metrics.recordExecutionCompleted(2ms);
    metrics.recordExecutionStarted();
    metrics.recordExecutionFailed(4ms);
    metrics.recordRetryScheduled();
    const auto snapshot = metrics.snapshot();
    std::ostringstream report;
    report << std::scientific << std::setprecision(1);
    const auto flags = report.flags();
    const auto precision = report.precision();
    jobq::cli::printInteractiveMetrics(snapshot, report);
    check(value(report.str(), "Submitted jobs") == "1" && value(report.str(), "Started executions") == "2" &&
          value(report.str(), "Completed executions") == "1" && value(report.str(), "Failed executions") == "1" &&
          value(report.str(), "Scheduled retries") == "1" && value(report.str(), "Finished executions") == "2" &&
          value(report.str(), "Average execution") == "3.000 ms" && value(report.str(), "Total execution") == "6.000 ms" &&
          snapshot.throughputJobsPerSecond > 0 && report.flags() == flags && report.precision() == precision,
          "Rendering uses actual counters/durations/throughput without changing caller formatting");
    auto fixture = snapshot;
    fixture.averageExecutionTime = 1500ns;
    fixture.totalExecutionTime = 2500ms;
    fixture.elapsedTime = 10s;
    fixture.throughputJobsPerSecond = 0.2;
    std::ostringstream units;
    jobq::cli::printInteractiveMetrics(fixture, units);
    check(value(units.str(), "Average execution") == "1.500 us" && value(units.str(), "Total execution") == "2.500 s" &&
          value(units.str(), "Metrics elapsed") == "10.000 s" &&
          value(units.str(), "Cumulative throughput") == "0.200 executions/s",
          "Deterministic fixtures verify duration scaling and cumulative rate precision");
}

void testConcurrentEngineMetrics() {
    jobq_test::Gate release;
    jobq_test::Counter entered;
    jobq::Engine engine({.workerCount = 1});
    engine.registerHandler("BLOCK", [&](const jobq::Job&) {
        entered.add();
        if (!release.wait()) throw std::runtime_error("Handler timed out");
    });
    engine.registerHandler("FAIL", [](const jobq::Job&) { throw std::runtime_error("expected"); });
    engine.start();
    const auto id = engine.submit("BLOCK", "");
    check(entered.waitAtLeast(1), "A genuine execution is active before snapshots");
    auto reader = std::async(std::launch::async, [&] {
        for (int index = 0; index < 100; ++index) {
            const auto snapshot = engine.metrics();
            std::ostringstream output;
            jobq::cli::printInteractiveMetrics(snapshot, output);
            if (snapshot.startedExecutions != 1 || snapshot.finishedExecutions != 0 ||
                snapshot.submittedJobs != 1 || value(output.str(), "Started executions") != "1") return false;
        }
        return true;
    });
    check(reader.wait_for(5s) == std::future_status::ready && reader.get() &&
          engine.getStatus(id) == jobq::JobStatus::Running,
          "Concurrent snapshot rendering does not alter the active job or counters");
    jobq::JobOptions options;
    options.retryPolicy.maxRetries = 1;
    options.retryPolicy.initialDelay = 0ms;
    engine.submit("FAIL", "", options);
    release.open();
    engine.stop();
    const auto final = engine.metrics();
    check(final.submittedJobs == 2 && final.startedExecutions == 3 && final.completedExecutions == 1 &&
          final.failedExecutions == 2 && final.finishedExecutions == 3 && final.scheduledRetries == 1 &&
          final.totalExecutionTime.count() >= 0 && final.averageExecutionTime.count() >= 0,
          "Metrics distinguish two submissions, three attempts, two failures, and one retry");
    check(engine.metrics().finishedExecutions == final.finishedExecutions,
          "Metrics stay readable and unchanged after worker shutdown");
}

void testCommandValidation() {
    jobq::cli::InteractiveRuntime runtime;
    std::ostringstream output;
    check(jobq_test::throwsWith<std::logic_error>([&] {
        runtime.commands().dispatch({"metrics", {}}, output);
    }, "initialized"), "Unavailable Engine metrics are reported rather than fabricated as zeros");
    std::istringstream input("help metrics\nmetrics extra\nmetrics\nstatus\nexit\n");
    std::ostringstream errors;
    check(runtime.run(input, output, errors) == 0 && errors.str().find("Usage: metrics") != std::string::npos &&
          output.str().find("JobQ Metrics (cumulative)") != std::string::npos &&
          output.str().find("JobQ Runtime Status") != std::string::npos,
          "Metrics validation errors preserve the runtime and later commands");
    runtime.commands().dispatch({"metrics", {}}, output);
    check(!runtime.engine().isRunning(), "Post-shutdown metrics do not restart the Engine");
}

}

int main() {
    try {
        testFormatting();
        testConcurrentEngineMetrics();
        testCommandValidation();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
