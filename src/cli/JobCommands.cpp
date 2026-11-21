#include "cli/JobCommands.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "cli/InteractiveRuntime.h"

namespace jobq::cli {

namespace {

template <typename Value>
void field(std::ostream& output, std::string_view name, const Value& value) {
    output << std::left << std::setw(24) << name << value << '\n';
}

JobId parseJobId(const InteractiveCommand& command) {
    if (command.arguments.size() != 1) {
        throw std::invalid_argument("Usage: job <job-id>");
    }
    const auto& text = command.arguments.front();
    JobId id = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (error != std::errc{} || end != text.data() + text.size() || id == 0) {
        throw std::invalid_argument("Usage: job <job-id> (positive decimal uint64 identifier)");
    }
    return id;
}

}

std::string summarizeJobText(std::string_view text) {
    constexpr std::size_t limit = 160;
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "\"";
    for (const unsigned char character : text.substr(0, limit)) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += static_cast<char>(character);
        } else if (character < 0x20 || character >= 0x7F) {
            result += "\\x";
            result += hex[character >> 4];
            result += hex[character & 15];
        } else {
            result += static_cast<char>(character);
        }
    }
    result += '"';
    if (text.size() > limit) {
        result += " ... [truncated; " + std::to_string(text.size()) + " bytes total]";
    }
    return result;
}

void printJobs(const std::vector<JobSnapshot>& jobs, std::ostream& output) {
    std::ostringstream report;
    report << "JobQ Jobs\n";
    if (jobs.empty()) {
        report << "No jobs available.\n";
    } else {
        report << "ID | TYPE | STATUS | PRIORITY | ATTEMPTS\n";
        for (const auto& job : jobs) {
            report << job.id << " | " << summarizeJobText(job.type) << " | "
                   << toString(job.status) << " | " << toString(job.priority) << " | "
                   << job.attemptsMade << '\n';
        }
        report << "Total jobs: " << jobs.size() << '\n';
    }
    output << report.str();
}

void printJob(const JobSnapshot& job, std::ostream& output) {
    std::ostringstream report;
    report << "Job Details\n";
    field(report, "ID", job.id);
    field(report, "Type", summarizeJobText(job.type));
    field(report, "Payload", summarizeJobText(job.payload));
    field(report, "Status", toString(job.status));
    field(report, "Priority", toString(job.priority));
    field(report, "Attempts made", job.attemptsMade);
    field(report, "Maximum retries", job.retryPolicy.maxRetries);
    field(report, "Initial retry delay (ms)", job.retryPolicy.initialDelay.count());
    field(report, "Retry multiplier", job.retryPolicy.multiplier);
    field(report, "Maximum retry delay (ms)", job.retryPolicy.maxDelay.count());
    field(report, "Lifecycle timestamps", "Unavailable");
    field(report, "Next execution time", "Unavailable");
    field(report, "Last error", "Unavailable");
    output << report.str();
}

void registerJobCommands(InteractiveRuntime& runtime) {
    runtime.commands().registerCommand("submit", "Submit a job to a registered handler; quote multi-word payloads.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            if (command.arguments.size() != 2 || command.arguments.front().empty()) {
                throw std::invalid_argument("Usage: submit <type> <payload>");
            }
            auto& engine = runtime.engine();
            const auto id = runtime.submitJob(command.arguments[0], command.arguments[1]);
            std::ostringstream report;
            report << "Job submitted successfully.\n";
            field(report, "Job ID", id);
            const auto job = engine.getJob(id);
            if (job) {
                field(report, "Type", summarizeJobText(job->type));
                field(report, "Status", toString(job->status));
            }
            output << report.str();
            return CommandAction::Continue;
        }, "submit <type> <payload>");
    runtime.commands().registerCommand("jobs", "List available jobs in ascending identifier order.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            if (!command.arguments.empty()) {
                throw std::invalid_argument("Usage: jobs (no arguments)");
            }
            printJobs(runtime.engine().listJobs(), output);
            return CommandAction::Continue;
        });
    runtime.commands().registerCommand("job", "Inspect one available job by its identifier.",
        [&runtime](const InteractiveCommand& command, std::ostream& output) {
            const auto id = parseJobId(command);
            const auto job = runtime.engine().getJob(id);
            if (!job) {
                throw std::invalid_argument("Job not found: " + std::to_string(id));
            }
            printJob(*job, output);
            return CommandAction::Continue;
        }, "job <job-id>");
}

}
