
#include "cli/CommandHandlers.h"

#include <charconv>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"

namespace jobq::cli {

namespace {

std::string lowercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(
                static_cast<unsigned char>(character)
            )
        );
    }

    return value;
}

std::string uppercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::toupper(
                static_cast<unsigned char>(character)
            )
        );
    }

    return value;
}

template <typename Integer>
bool parseUnsigned(
    const std::string& value,
    Integer& result
) {
    if (value.empty()) {
        return false;
    }

    const char* begin = value.data();
    const char* end = begin + value.size();

    const auto [position, error] =
        std::from_chars(begin, end, result);

    return error == std::errc{} && position == end;
}

int usageError(
    const std::string& message,
    std::ostream& errorOutput
) {
    errorOutput << "Error: " << message << '\n';

    return kUsageErrorExitCode;
}

std::filesystem::path resolveStoragePath(
    const ParsedCommandLine& parsed
) {
    const auto iterator = parsed.options.find("storage");

    if (iterator == parsed.options.end()) {
        return "data/jobs.journal";
    }

    return iterator->second;
}

bool isValidStatus(const std::string& status) {
    return status == "PENDING" ||
           status == "RUNNING" ||
           status == "COMPLETED" ||
           status == "FAILED" ||
           status == "CANCELLED";
}

bool hasStatus(
    const JobRecord& record,
    const std::string& status
) {
    return toString(record.status) == status;
}

std::string escapeForDisplay(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (const unsigned char character : value) {
        switch (character) {
            case '\n':
                result += "\\n";
                break;

            case '\r':
                result += "\\r";
                break;

            case '\t':
                result += "\\t";
                break;

            case '\\':
                result += "\\\\";
                break;

            default:
                if (character == 0) {
                    result += "\\0";
                } else if (character < 32 || character == 127) {
                    result += '?';
                } else {
                    result += static_cast<char>(character);
                }
                break;
        }
    }

    return result;
}

void printJobSummary(
    const JobRecord& record,
    std::ostream& output
) {
    output
        << "ID: " << record.id
        << " | Type: " << record.type
        << " | Status: " << toString(record.status)
        << " | Priority: " << toString(record.priority)
        << " | Attempts: " << record.attemptsMade
        << " | Max retries: " << record.retryPolicy.maxRetries
        << " | Payload: " << escapeForDisplay(record.payload)
        << '\n';
}

void printJobDetails(
    const JobRecord& record,
    std::ostream& output
) {
    output << "Job details\n";
    output << "-----------\n";
    output << "ID: " << record.id << '\n';
    output << "Type: " << record.type << '\n';
    output << "Payload: "
           << escapeForDisplay(record.payload) << '\n';
    output << "Status: " << toString(record.status) << '\n';
    output << "Priority: " << toString(record.priority) << '\n';
    output << "Attempts made: " << record.attemptsMade << '\n';
    output << "Maximum retries: "
           << record.retryPolicy.maxRetries << '\n';
    output << "Retry initial delay (ms): "
           << record.retryPolicy.initialDelay.count() << '\n';
    output << "Retry multiplier: "
           << record.retryPolicy.multiplier << '\n';
    output << "Retry maximum delay (ms): "
           << record.retryPolicy.maxDelay.count() << '\n';
}

} // namespace

int executeSubmitCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
) {
    if (parsed.command != "submit") {
        return usageError(
            "Submit handler received a different command.",
            errorOutput
        );
    }

    const auto typeIterator = parsed.options.find("type");
    const auto payloadIterator = parsed.options.find("payload");

    if (typeIterator == parsed.options.end()) {
        return usageError(
            "Missing required option --type.",
            errorOutput
        );
    }

    if (payloadIterator == parsed.options.end()) {
        return usageError(
            "Missing required option --payload.",
            errorOutput
        );
    }

    if (typeIterator->second.empty()) {
        return usageError(
            "Job type cannot be empty.",
            errorOutput
        );
    }

    const std::string type = typeIterator->second;
    const std::string payload = payloadIterator->second;

    JobPriority priority = JobPriority::Normal;

    const auto priorityIterator =
        parsed.options.find("priority");

    if (priorityIterator != parsed.options.end()) {
        const std::string value =
            lowercase(priorityIterator->second);

        if (value == "low") {
            priority = JobPriority::Low;
        } else if (value == "normal") {
            priority = JobPriority::Normal;
        } else if (value == "high") {
            priority = JobPriority::High;
        } else {
            return usageError(
                "Invalid priority. Use low, normal, or high.",
                errorOutput
            );
        }
    }

    std::size_t maxRetries = 0;

    const auto retriesIterator =
        parsed.options.find("retries");

    if (retriesIterator != parsed.options.end()) {
        if (!parseUnsigned(
                retriesIterator->second,
                maxRetries
            )) {
            return usageError(
                "Invalid --retries value. Use a non-negative integer.",
                errorOutput
            );
        }
    }

    const auto storagePath = resolveStoragePath(parsed);

    if (storagePath.empty()) {
        return usageError(
            "Storage path cannot be empty.",
            errorOutput
        );
    }

    RetryPolicy retryPolicy;
    retryPolicy.maxRetries = maxRetries;

    try {
        JobQueue queue;

        auto storage =
            std::make_shared<FileJobStorage>(storagePath);

        JobManager manager(queue, storage);

        auto job = manager.submit(
            type,
            payload,
            priority,
            retryPolicy
        );

        output << "Job submitted successfully.\n";
        output << "ID: " << job->getId() << '\n';
        output << "Type: " << job->getType() << '\n';
        output << "Priority: "
               << toString(job->getPriority()) << '\n';
        output << "Maximum retries: "
               << job->getRetryPolicy().maxRetries << '\n';
        output << "Status: "
               << toString(job->getStatus()) << '\n';
        output << "Storage: " << storagePath.string() << '\n';

        return kSuccessExitCode;
    } catch (const std::exception& exception) {
        errorOutput
            << "Submission failed: "
            << exception.what()
            << '\n';

        return kCommandErrorExitCode;
    }
}

int executeListCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
) {
    if (parsed.command != "list") {
        return usageError(
            "List handler received a different command.",
            errorOutput
        );
    }

    if (!parsed.positionalArguments.empty()) {
        return usageError(
            "The list command does not accept positional arguments.",
            errorOutput
        );
    }

    std::optional<std::string> statusFilter;

    const auto statusIterator =
        parsed.options.find("status");

    if (statusIterator != parsed.options.end()) {
        statusFilter = uppercase(statusIterator->second);

        if (!isValidStatus(*statusFilter)) {
            return usageError(
                "Invalid status. Use pending, running, completed, failed, or cancelled.",
                errorOutput
            );
        }
    }

    std::optional<std::size_t> limit;

    const auto limitIterator =
        parsed.options.find("limit");

    if (limitIterator != parsed.options.end()) {
        std::size_t parsedLimit = 0;

        if (!parseUnsigned(
                limitIterator->second,
                parsedLimit
            ) ||
            parsedLimit == 0) {
            return usageError(
                "Invalid --limit value. Use a positive integer.",
                errorOutput
            );
        }

        limit = parsedLimit;
    }

    const auto storagePath = resolveStoragePath(parsed);

    if (storagePath.empty()) {
        return usageError(
            "Storage path cannot be empty.",
            errorOutput
        );
    }

    try {
        FileJobStorage storage(storagePath);

        const auto records = storage.loadAll();

        std::size_t displayed = 0;

        for (const auto& record : records) {
            if (statusFilter &&
                !hasStatus(record, *statusFilter)) {
                continue;
            }

            if (limit && displayed >= *limit) {
                break;
            }

            printJobSummary(record, output);
            ++displayed;
        }

        if (displayed == 0) {
            output << "No jobs found.\n";
        } else {
            output << "Total displayed: " << displayed << '\n';
        }

        return kSuccessExitCode;
    } catch (const std::exception& exception) {
        errorOutput
            << "Unable to list jobs: "
            << exception.what()
            << '\n';

        return kCommandErrorExitCode;
    }
}

int executeStatusCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
) {
    if (parsed.command != "status") {
        return usageError(
            "Status handler received a different command.",
            errorOutput
        );
    }

    if (parsed.positionalArguments.size() != 1) {
        return usageError(
            "Usage: jobq_cli status JOB_ID [--storage PATH]",
            errorOutput
        );
    }

    std::uint64_t jobId = 0;

    if (!parseUnsigned(
            parsed.positionalArguments.front(),
            jobId
        ) ||
        jobId == 0) {
        return usageError(
            "Job ID must be a positive integer.",
            errorOutput
        );
    }

    const auto storagePath = resolveStoragePath(parsed);

    if (storagePath.empty()) {
        return usageError(
            "Storage path cannot be empty.",
            errorOutput
        );
    }

    try {
        FileJobStorage storage(storagePath);

        const auto records = storage.loadAll();

        const auto iterator = std::find_if(
            records.begin(),
            records.end(),
            [jobId](const JobRecord& record) {
                return record.id == jobId;
            }
        );

        if (iterator == records.end()) {
            errorOutput
                << "Job not found: "
                << jobId
                << '\n';

            return kCommandErrorExitCode;
        }

        printJobDetails(*iterator, output);

        return kSuccessExitCode;
    } catch (const std::exception& exception) {
        errorOutput
            << "Unable to read job status: "
            << exception.what()
            << '\n';

        return kCommandErrorExitCode;
    }
}

}
