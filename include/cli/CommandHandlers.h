#pragma once

#include <chrono>
#include <iosfwd>
#include <string>

#include "cli/CommandLine.h"

namespace jobq {
class JobMetrics;
struct JobMetricsSnapshot;
}

namespace jobq::cli {

std::string formatDuration(std::chrono::nanoseconds duration);

std::string formatThroughput(double throughput);

std::string formatMetrics(const JobMetricsSnapshot& snapshot);

void printMetrics(
    const JobMetricsSnapshot& snapshot,
    std::ostream& output
);

int executeSubmitCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeListCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeStatusCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeRunCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeMetricsCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput,
    const JobMetrics* metrics = nullptr
);

} // namespace jobq::cli
