
#include "cli/CommandHandlers.h"

#include <charconv>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"

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

bool parseRetryCount(
    const std::string& value,
    std::size_t& result
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

    jobq::JobPriority priority = jobq::JobPriority::Normal;

    const auto priorityIterator =
        parsed.options.find("priority");

    if (priorityIterator != parsed.options.end()) {
        const std::string value =
            lowercase(priorityIterator->second);

        if (value == "low") {
            priority = jobq::JobPriority::Low;
        } else if (value == "normal") {
            priority = jobq::JobPriority::Normal;
        } else if (value == "high") {
            priority = jobq::JobPriority::High;
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
        if (!parseRetryCount(
                retriesIterator->second,
                maxRetries
            )) {
            return usageError(
                "Invalid --retries value. Use a non-negative integer.",
                errorOutput
            );
        }
    }

    std::filesystem::path storagePath = "data/jobs.journal";

    const auto storageIterator =
        parsed.options.find("storage");

    if (storageIterator != parsed.options.end()) {
        if (storageIterator->second.empty()) {
            return usageError(
                "Storage path cannot be empty.",
                errorOutput
            );
        }

        storagePath = storageIterator->second;
    }

    jobq::RetryPolicy retryPolicy;
    retryPolicy.maxRetries = maxRetries;

    try {
        jobq::JobQueue queue;

        auto storage =
            std::make_shared<jobq::FileJobStorage>(storagePath);

        jobq::JobManager manager(queue, storage);

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
               << jobq::toString(job->getPriority()) << '\n';
        output << "Maximum retries: "
               << job->getRetryPolicy().maxRetries << '\n';
        output << "Status: "
               << jobq::toString(job->getStatus()) << '\n';
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

}
