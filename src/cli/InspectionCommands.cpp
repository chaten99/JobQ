#include "cli/InspectionCommands.h"

#include <chrono>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "cli/InteractiveRuntime.h"

namespace jobq::cli {

namespace {

std::string_view stateName(EngineState state) {
    switch (state) {
        case EngineState::Created: return "Created";
        case EngineState::Starting: return "Starting";
        case EngineState::Running: return "Running";
        case EngineState::Stopping: return "Stopping";
        case EngineState::Stopped: return "Stopped";
    }
    return "Unavailable";
}

std::string_view stateName(RuntimeState state) {
    switch (state) {
        case RuntimeState::Created: return "Created";
        case RuntimeState::Starting: return "Starting";
        case RuntimeState::Running: return "Running";
        case RuntimeState::Stopping: return "Stopping";
        case RuntimeState::Stopped: return "Stopped";
        case RuntimeState::Failed: return "Failed";
    }
    return "Unavailable";
}

template <typename Value>
void field(std::ostream& output, std::string_view name, const Value& value) {
    output << std::left << std::setw(24) << name << value << '\n';
}

std::string uptime(std::chrono::nanoseconds elapsed) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
           << std::setw(2) << seconds / 60 % 60 << ':' << std::setw(2) << seconds % 60;
    return output.str();
}

std::string_view workerState(const EngineSnapshot& snapshot) {
    if (snapshot.workersStopped) {
        return "Stopped";
    }
    if (snapshot.workersStopping) {
        return "Stopping";
    }
    return snapshot.workersStarted ? "Running" : "Created";
}

void requireNoArguments(const InteractiveCommand& command) {
    if (!command.arguments.empty()) {
        throw std::invalid_argument("Usage: " + command.name + " (no arguments)");
    }
}

}

void printRuntimeStatus(const RuntimeSnapshot& snapshot, std::ostream& output) {
    std::ostringstream report;
    report << "JobQ Runtime Status\n";
    field(report, "Runtime", stateName(snapshot.state));
    if (snapshot.engine) {
        field(report, "Engine", stateName(snapshot.engine->state));
        field(report, "Configured workers", snapshot.engine->workerCount);
        field(report, "Submitted jobs", snapshot.engine->metrics.submittedJobs);
    } else {
        field(report, "Engine", "Unavailable");
        field(report, "Configured workers", "Unavailable");
        field(report, "Submitted jobs", "Unavailable");
    }
    field(report, "Uptime", snapshot.uptime ? uptime(*snapshot.uptime) : "Unavailable");
    output << report.str();
}

void printWorkers(const RuntimeSnapshot& snapshot, std::ostream& output) {
    std::ostringstream report;
    report << "JobQ Workers\n";
    if (snapshot.engine) {
        const auto& engine = *snapshot.engine;
        field(report, "Worker pool", workerState(engine));
        field(report, "Configured workers", engine.workerCount);
    } else {
        field(report, "Worker pool", "Unavailable");
        field(report, "Configured workers", "Unavailable");
    }
    field(report, "Active/idle workers", "Unavailable");
    field(report, "Per-worker details", "Unavailable");
    output << report.str();
}

void printQueue(const RuntimeSnapshot& snapshot, std::ostream& output) {
    std::ostringstream report;
    report << "JobQ Queue\n";
    if (snapshot.engine) {
        const auto& engine = *snapshot.engine;
        field(report, "Queue state", engine.queueClosed ? "Closed" : "Open");
        field(report, "Ready waiting", engine.readyJobs);
        field(report, "Retry waiting", engine.delayedJobs);
        field(report, "Waiting total", engine.readyJobs + engine.delayedJobs);
        field(report, "Completed executions", engine.metrics.completedExecutions);
        field(report, "Failed executions", engine.metrics.failedExecutions);
    } else {
        field(report, "Queue state", "Unavailable");
        field(report, "Waiting counts", "Unavailable");
        field(report, "Execution counters", "Unavailable");
    }
    output << report.str();
}

void registerInspectionCommands(InteractiveRuntime& runtime) {
    runtime.commands().registerCommand("status", "Display runtime status.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            requireNoArguments(command);
            printRuntimeStatus(runtime.snapshot(), output);
            return CommandAction::Continue;
        });
    runtime.commands().registerCommand("workers", "Display worker pool information.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            requireNoArguments(command);
            printWorkers(runtime.snapshot(), output);
            return CommandAction::Continue;
        });
    runtime.commands().registerCommand("queue", "Display waiting jobs and execution counters.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            requireNoArguments(command);
            printQueue(runtime.snapshot(), output);
            return CommandAction::Continue;
        });
}

}
