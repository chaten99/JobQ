#include "cli/MetricsCommands.h"

#include <charconv>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "cli/InteractiveRuntime.h"

namespace jobq::cli {

namespace {

constexpr std::string_view monitorUsage =
    "Usage: monitor [<interval-ms> <sample-count>] (interval 1..10000 ms, samples 1..60, total <= 60000 ms)";

void validateOptions(const MonitorOptions& options) {
    if (options.interval.count() < 1 || options.interval.count() > 10000 ||
        options.sampleCount < 1 || options.sampleCount > 60 ||
        options.interval.count() > 60000 / static_cast<std::chrono::milliseconds::rep>(options.sampleCount)) {
        throw std::invalid_argument(std::string(monitorUsage));
    }
}

std::uint64_t unsignedArgument(const std::string& text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument(std::string(monitorUsage));
    }
    return value;
}

template <typename Value>
void field(std::ostream& output, std::string_view name, const Value& value) {
    output << std::left << std::setw(28) << name << value << '\n';
}

std::string durationText(std::chrono::nanoseconds value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(3);
    if (value >= std::chrono::seconds{1}) {
        output << std::chrono::duration<double>(value).count() << " s";
    } else if (value >= std::chrono::milliseconds{1}) {
        output << std::chrono::duration<double, std::milli>(value).count() << " ms";
    } else if (value >= std::chrono::microseconds{1}) {
        output << std::chrono::duration<double, std::micro>(value).count() << " us";
    } else {
        output << value.count() << " ns";
    }
    return output.str();
}

std::string rateText(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(3) << value << " executions/s";
    return output.str();
}

void requireOutput(std::ostream& output) {
    output.flush();
    if (!output) {
        throw std::runtime_error("Unable to write metrics output");
    }
}

void printInterval(const std::optional<MetricsInterval>& interval, std::ostream& output) {
    std::ostringstream report;
    report << "Interval changes\n";
    if (interval) {
        field(report, "Submitted jobs", interval->submittedJobs);
        field(report, "Started executions", interval->startedExecutions);
        field(report, "Completed executions", interval->completedExecutions);
        field(report, "Failed executions", interval->failedExecutions);
        field(report, "Scheduled retries", interval->scheduledRetries);
        field(report, "Finished executions", interval->finishedExecutions);
        field(report, "Measured interval", durationText(interval->elapsed));
        field(report, "Finished execution rate", rateText(interval->finishedExecutionsPerSecond));
    } else {
        report << "Unavailable: counter/time reset, inconsistent sample, or nonpositive measured interval.\n";
    }
    output << report.str();
}

}

MonitorOptions parseMonitorOptions(const InteractiveCommand& command) {
    MonitorOptions options;
    if (!command.arguments.empty()) {
        if (command.arguments.size() != 2) {
            throw std::invalid_argument(std::string(monitorUsage));
        }
        const auto interval = unsignedArgument(command.arguments[0]);
        const auto samples = unsignedArgument(command.arguments[1]);
        if (interval > 10000 || samples > 60) {
            throw std::invalid_argument(std::string(monitorUsage));
        }
        options.interval = std::chrono::milliseconds{interval};
        options.sampleCount = static_cast<std::size_t>(samples);
    }
    validateOptions(options);
    return options;
}

std::optional<MetricsInterval> calculateMetricsInterval(
    const JobMetricsSnapshot& previous, const JobMetricsSnapshot& current,
    std::chrono::nanoseconds elapsed) {
    if (elapsed <= std::chrono::nanoseconds::zero() ||
        current.submittedJobs < previous.submittedJobs ||
        current.startedExecutions < previous.startedExecutions ||
        current.completedExecutions < previous.completedExecutions ||
        current.failedExecutions < previous.failedExecutions ||
        current.scheduledRetries < previous.scheduledRetries ||
        current.finishedExecutions < previous.finishedExecutions ||
        previous.finishedExecutions < previous.completedExecutions ||
        previous.finishedExecutions - previous.completedExecutions != previous.failedExecutions ||
        current.finishedExecutions < current.completedExecutions ||
        current.finishedExecutions - current.completedExecutions != current.failedExecutions ||
        current.elapsedTime < previous.elapsedTime ||
        current.totalExecutionTime < previous.totalExecutionTime) {
        return std::nullopt;
    }
    MetricsInterval result;
    result.submittedJobs = current.submittedJobs - previous.submittedJobs;
    result.startedExecutions = current.startedExecutions - previous.startedExecutions;
    result.completedExecutions = current.completedExecutions - previous.completedExecutions;
    result.failedExecutions = current.failedExecutions - previous.failedExecutions;
    result.scheduledRetries = current.scheduledRetries - previous.scheduledRetries;
    result.finishedExecutions = current.finishedExecutions - previous.finishedExecutions;
    result.elapsed = elapsed;
    result.finishedExecutionsPerSecond = static_cast<double>(result.finishedExecutions) /
                                        std::chrono::duration<double>(elapsed).count();
    return result;
}

void printInteractiveMetrics(const JobMetricsSnapshot& snapshot, std::ostream& output) {
    std::ostringstream report;
    report << "JobQ Metrics (cumulative)\n";
    field(report, "Submitted jobs", snapshot.submittedJobs);
    field(report, "Started executions", snapshot.startedExecutions);
    field(report, "Completed executions", snapshot.completedExecutions);
    field(report, "Failed executions", snapshot.failedExecutions);
    field(report, "Scheduled retries", snapshot.scheduledRetries);
    field(report, "Finished executions", snapshot.finishedExecutions);
    field(report, "Average execution", durationText(snapshot.averageExecutionTime));
    field(report, "Total execution", durationText(snapshot.totalExecutionTime));
    field(report, "Metrics elapsed", durationText(snapshot.elapsedTime));
    field(report, "Cumulative throughput", rateText(snapshot.throughputJobsPerSecond));
    output << report.str();
}

void monitorMetrics(Engine& engine, MonitorOptions options, std::ostream& output, MonitorTiming timing) {
    validateOptions(options);
    if (!engine.isRunning()) {
        throw std::logic_error("Monitoring requires a running engine");
    }
    if (!timing.now) {
        timing.now = [] { return std::chrono::steady_clock::now(); };
    }
    if (!timing.wait) {
        timing.wait = [](std::chrono::milliseconds interval) { std::this_thread::sleep_for(interval); };
    }
    auto previous = engine.metrics();
    auto previousAt = timing.now();
    output << "Monitoring JobQ\nBaseline\n";
    printInteractiveMetrics(previous, output);
    requireOutput(output);
    for (std::size_t sample = 1; sample <= options.sampleCount; ++sample) {
        if (!engine.isRunning()) {
            output << "Monitoring stopped: Engine is not running.\n";
            return;
        }
        timing.wait(options.interval);
        if (!engine.isRunning()) {
            output << "Monitoring stopped: Engine is not running.\n";
            return;
        }
        const auto current = engine.metrics();
        const auto currentAt = timing.now();
        output << "\nSample " << sample << '\n';
        printInteractiveMetrics(current, output);
        printInterval(calculateMetricsInterval(previous, current,
            std::chrono::duration_cast<std::chrono::nanoseconds>(currentAt - previousAt)), output);
        requireOutput(output);
        previous = current;
        previousAt = currentAt;
    }
    output << "Monitoring completed.\n";
    requireOutput(output);
}

void registerMetricsCommands(InteractiveRuntime& runtime) {
    runtime.commands().registerCommand("metrics", "Display cumulative Engine processing metrics.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            if (!command.arguments.empty()) {
                throw std::invalid_argument("Usage: metrics (no arguments)");
            }
            printInteractiveMetrics(runtime.engine().metrics(), output);
            requireOutput(output);
            return CommandAction::Continue;
        });
    runtime.commands().registerCommand("monitor", "Sample metrics (defaults: 1000 ms, 5 samples; bounded to 60 s of waits).",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            const auto options = parseMonitorOptions(command);
            monitorMetrics(runtime.engine(), options, output);
            return CommandAction::Continue;
        }, "monitor [<interval-ms> <sample-count>]");
}

}
