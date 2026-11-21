#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>

#include "cli/InteractiveCommand.h"
#include "metrics/JobMetrics.h"

namespace jobq {
class Engine;
}

namespace jobq::cli {

class InteractiveRuntime;

struct MonitorOptions {
    std::chrono::milliseconds interval{1000};
    std::size_t sampleCount{5};
};

struct MonitorTiming {
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> wait;
};

struct MetricsInterval {
    std::uint64_t submittedJobs{0};
    std::uint64_t startedExecutions{0};
    std::uint64_t completedExecutions{0};
    std::uint64_t failedExecutions{0};
    std::uint64_t scheduledRetries{0};
    std::uint64_t finishedExecutions{0};
    std::chrono::nanoseconds elapsed{0};
    double finishedExecutionsPerSecond{0.0};
};

MonitorOptions parseMonitorOptions(const InteractiveCommand& command);
std::optional<MetricsInterval> calculateMetricsInterval(
    const JobMetricsSnapshot& previous, const JobMetricsSnapshot& current,
    std::chrono::nanoseconds elapsed);
void printInteractiveMetrics(const JobMetricsSnapshot& snapshot, std::ostream& output);
void monitorMetrics(Engine& engine, MonitorOptions options, std::ostream& output,
                    MonitorTiming timing = {});
void registerMetricsCommands(InteractiveRuntime& runtime);

}
