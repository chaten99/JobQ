#include "cli/CommandHandlers.h"
#include "cli/CommandLine.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"

namespace {

int testsRun = 0;
int testsFailed = 0;

void check(bool condition, const std::string& description) {
    ++testsRun;
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

jobq::cli::ParsedCommandLine runCommand(
    const std::filesystem::path& storagePath,
    const std::string& workerCount = "3"
) {
    jobq::cli::ParsedCommandLine parsed;
    parsed.command = "run";
    parsed.options["storage"] = storagePath.string();
    if (!workerCount.empty()) {
        parsed.options["workers"] = workerCount;
    }
    return parsed;
}

const jobq::JobRecord* findRecord(
    const std::vector<jobq::JobRecord>& records,
    const std::string& type
) {
    for (const auto& record : records) {
        if (record.type == type) {
            return &record;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    const auto testDirectory =
        std::filesystem::temp_directory_path() / "jobq_cli_run_tests";
    const auto storagePath = testDirectory / "jobs.journal";

    std::error_code filesystemError;
    std::filesystem::remove_all(testDirectory, filesystemError);
    std::filesystem::create_directories(testDirectory);

    auto storage = std::make_shared<jobq::FileJobStorage>(storagePath);

    {
        jobq::JobQueue queue;
        jobq::JobManager manager(queue, storage);
        jobq::RetryPolicy noRetries;
        noRetries.maxRetries = 0;

        manager.submit(
            "EMAIL",
            "cli-run-email",
            jobq::JobPriority::Normal,
            noRetries
        );
        manager.submit(
            "FAIL",
            "cli-run-intentional-failure",
            jobq::JobPriority::Normal,
            noRetries
        );
        manager.submit(
            "UNKNOWN",
            "cli-run-missing-handler",
            jobq::JobPriority::Normal,
            noRetries
        );
    }

    {
        auto parsed = runCommand(storagePath);
        std::ostringstream output;
        std::ostringstream errorOutput;

        const int result = jobq::cli::executeRunCommand(
            parsed,
            output,
            errorOutput
        );

        check(
            result == jobq::cli::kSuccessExitCode,
            "run command executes successfully despite individual job failures"
        );
        check(
            output.str().find("Starting JobQ with 3 workers") != std::string::npos,
            "run command starts the requested number of workers"
        );
        check(
            output.str().find("JobQ stopped cleanly.") != std::string::npos,
            "run command waits for workers and shuts down cleanly"
        );
        check(
            output.str().find("Completed jobs: 1") != std::string::npos,
            "run command reports completed jobs"
        );
        check(
            output.str().find("Failed jobs: 2") != std::string::npos,
            "run command reports failed jobs"
        );
        check(
            errorOutput.str().empty(),
            "job-level failures do not become CLI execution errors"
        );
    }

    const auto records = storage->loadAll();
    const auto* emailRecord = findRecord(records, "EMAIL");
    const auto* failRecord = findRecord(records, "FAIL");
    const auto* unknownRecord = findRecord(records, "UNKNOWN");

    check(
        emailRecord != nullptr &&
            emailRecord->status == jobq::JobStatus::Completed,
        "completed status is persisted to the journal"
    );
    check(
        failRecord != nullptr &&
            failRecord->status == jobq::JobStatus::Failed,
        "intentional handler failure status is persisted"
    );
    check(
        unknownRecord != nullptr &&
            unknownRecord->status == jobq::JobStatus::Failed,
        "jobs without a registered handler are persisted as failed"
    );

    {
        auto parsed = runCommand(storagePath, "0");
        std::ostringstream output;
        std::ostringstream errorOutput;
        const int result = jobq::cli::executeRunCommand(
            parsed,
            output,
            errorOutput
        );
        check(
            result == jobq::cli::kUsageErrorExitCode,
            "run rejects a zero worker count"
        );
    }

    {
        auto parsed = runCommand(storagePath, "not-a-number");
        std::ostringstream output;
        std::ostringstream errorOutput;
        const int result = jobq::cli::executeRunCommand(
            parsed,
            output,
            errorOutput
        );
        check(
            result == jobq::cli::kUsageErrorExitCode,
            "run rejects a non-numeric worker count"
        );
    }

    {
        auto parsed = runCommand(storagePath);
        parsed.positionalArguments.push_back("unexpected");
        std::ostringstream output;
        std::ostringstream errorOutput;
        const int result = jobq::cli::executeRunCommand(
            parsed,
            output,
            errorOutput
        );
        check(
            result == jobq::cli::kUsageErrorExitCode,
            "run rejects unexpected positional arguments"
        );
    }

    {
        auto parsed = runCommand(storagePath);
        parsed.options["storage"] = "";
        std::ostringstream output;
        std::ostringstream errorOutput;
        const int result = jobq::cli::executeRunCommand(
            parsed,
            output,
            errorOutput
        );
        check(
            result == jobq::cli::kUsageErrorExitCode,
            "run rejects an empty storage path"
        );
    }

    std::filesystem::remove_all(testDirectory, filesystemError);

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0
        ? "Result: ALL TESTS PASSED\n"
        : "Result: TEST FAILURES DETECTED\n");

    return testsFailed == 0 ? 0 : 1;
}
