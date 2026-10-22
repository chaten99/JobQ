
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "cli/CommandHandlers.h"
#include "cli/CommandLine.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"

namespace {

using namespace std::chrono_literals;

int testsRun = 0;
int testsFailed = 0;

const std::filesystem::path& testRoot() {
    static const auto root =
        std::filesystem::temp_directory_path() /
        "jobq_cli_inspection_tests";

    return root;
}

std::filesystem::path journalPath() {
    return testRoot() / "jobs.journal";
}

void resetStorage() {
    std::filesystem::remove_all(testRoot());
}

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

jobq::JobRecord makeRecord(
    uint64_t id,
    std::string type,
    std::string payload,
    jobq::JobStatus status,
    std::size_t attempts,
    jobq::JobPriority priority,
    jobq::RetryPolicy retryPolicy
) {
    jobq::JobRecord record;

    record.id = id;
    record.type = std::move(type);
    record.payload = std::move(payload);
    record.status = status;
    record.attemptsMade = attempts;
    record.priority = priority;
    record.retryPolicy = retryPolicy;

    return record;
}

void seedStorage() {
    resetStorage();

    jobq::FileJobStorage storage(journalPath());

    storage.save(
        makeRecord(
            101,
            "EMAIL",
            "welcome\nemail",
            jobq::JobStatus::Pending,
            0,
            jobq::JobPriority::High,
            jobq::RetryPolicy{3, 50ms, 2.0, 500ms}
        )
    );

    storage.save(
        makeRecord(
            102,
            "REPORT",
            "completed report",
            jobq::JobStatus::Completed,
            1,
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{}
        )
    );

    storage.save(
        makeRecord(
            103,
            "EMAIL",
            "first failure",
            jobq::JobStatus::Failed,
            1,
            jobq::JobPriority::Low,
            jobq::RetryPolicy{}
        )
    );

    storage.save(
        makeRecord(
            104,
            "REPORT",
            "second failure",
            jobq::JobStatus::Failed,
            2,
            jobq::JobPriority::Normal,
            jobq::RetryPolicy{1, 50ms, 2.0, 500ms}
        )
    );
}

int executeArgs(
    const std::vector<std::string>& arguments,
    std::ostringstream& output,
    std::ostringstream& errorOutput
) {
    jobq::cli::ParsedCommandLine parsed;
    std::string parseError;

    if (!jobq::cli::parseCommandLine(
            arguments,
            parsed,
            parseError
        )) {
        errorOutput << parseError << '\n';

        return jobq::cli::kUsageErrorExitCode;
    }

    if (parsed.command == "list") {
        return jobq::cli::executeListCommand(
            parsed,
            output,
            errorOutput
        );
    }

    if (parsed.command == "status") {
        return jobq::cli::executeStatusCommand(
            parsed,
            output,
            errorOutput
        );
    }

    return jobq::cli::kNotImplementedExitCode;
}

void testListAllJobs() {
    std::cout << "\n--- List All Jobs ---\n";

    seedStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = executeArgs(
        {
            "list",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    const auto text = output.str();

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "List command succeeds"
    );

    check(
        text.find("ID: 101") != std::string::npos &&
            text.find("ID: 102") != std::string::npos &&
            text.find("ID: 103") != std::string::npos &&
            text.find("ID: 104") != std::string::npos,
        "All stored jobs are displayed"
    );

    check(
        text.find("Status: PENDING") != std::string::npos &&
            text.find("Status: COMPLETED") != std::string::npos &&
            text.find("Status: FAILED") != std::string::npos,
        "List output displays persisted statuses"
    );

    check(
        text.find("welcome\\nemail") != std::string::npos,
        "Multiline payload is escaped in list output"
    );

    check(
        text.find("Total displayed: 4") != std::string::npos,
        "Displayed job count is correct"
    );

    check(
        error.str().empty(),
        "Successful listing emits no error"
    );
}

void testFilteredListAndLimit() {
    std::cout << "\n--- Status Filter and Limit ---\n";

    seedStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = executeArgs(
        {
            "list",
            "--status",
            "failed",
            "--limit",
            "1",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    const auto text = output.str();

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "Filtered list command succeeds"
    );

    check(
        text.find("ID: 103") != std::string::npos,
        "Matching FAILED job is included"
    );

    check(
        text.find("ID: 104") == std::string::npos,
        "Limit restricts the number of displayed records"
    );

    check(
        text.find("Total displayed: 1") != std::string::npos,
        "Filtered display count respects the limit"
    );
}

void testStatusForExistingJob() {
    std::cout << "\n--- Job Status Details ---\n";

    seedStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = executeArgs(
        {
            "status",
            "101",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    const auto text = output.str();

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "Status command succeeds for an existing ID"
    );

    check(
        text.find("ID: 101") != std::string::npos &&
            text.find("Type: EMAIL") != std::string::npos,
        "Status output contains ID and type"
    );

    check(
        text.find("Status: PENDING") != std::string::npos &&
            text.find("Priority: HIGH") != std::string::npos,
        "Status output contains status and priority"
    );

    check(
        text.find("Attempts made: 0") != std::string::npos &&
            text.find("Maximum retries: 3") != std::string::npos,
        "Status output contains attempts and retry limit"
    );

    check(
        text.find("Retry initial delay (ms): 50") !=
            std::string::npos,
        "Status output displays the retry timing policy"
    );

    check(
        text.find("welcome\\nemail") != std::string::npos,
        "Payload is displayed safely on one line"
    );
}

void testUnknownAndInvalidStatusQueries() {
    std::cout << "\n--- Unknown and Invalid Queries ---\n";

    seedStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int missingIdExit = executeArgs(
        {
            "status",
            "999",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        missingIdExit == jobq::cli::kCommandErrorExitCode,
        "Unknown job ID returns a command error"
    );

    check(
        error.str().find("Job not found: 999") !=
            std::string::npos,
        "Unknown job produces a readable message"
    );

    output.str("");
    output.clear();
    error.str("");
    error.clear();

    const int invalidIdExit = executeArgs(
        {
            "status",
            "abc",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        invalidIdExit == jobq::cli::kUsageErrorExitCode,
        "Non-numeric job ID is rejected"
    );

    output.str("");
    output.clear();
    error.str("");
    error.clear();

    const int missingId = executeArgs(
        {
            "status",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        missingId == jobq::cli::kUsageErrorExitCode,
        "Missing positional job ID is rejected"
    );
}

void testInvalidListOptions() {
    std::cout << "\n--- Invalid List Options ---\n";

    seedStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int invalidStatus = executeArgs(
        {
            "list",
            "--status",
            "WAITING",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        invalidStatus == jobq::cli::kUsageErrorExitCode,
        "Unsupported status filter is rejected"
    );

    output.str("");
    output.clear();
    error.str("");
    error.clear();

    const int invalidLimit = executeArgs(
        {
            "list",
            "--limit",
            "0",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        invalidLimit == jobq::cli::kUsageErrorExitCode,
        "Zero display limit is rejected"
    );

    output.str("");
    output.clear();
    error.str("");
    error.clear();

    const int invalidPositionals = executeArgs(
        {
            "list",
            "unexpected",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        invalidPositionals == jobq::cli::kUsageErrorExitCode,
        "Unexpected positional arguments are rejected"
    );
}

void testMissingStorageFile() {
    std::cout << "\n--- Missing Storage File ---\n";

    resetStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int listExit = executeArgs(
        {
            "list",
            "--storage",
            journalPath().string()
        },
        output,
        error
    );

    check(
        listExit == jobq::cli::kSuccessExitCode,
        "Listing a missing storage file succeeds"
    );

    check(
        output.str().find("No jobs found.") != std::string::npos,
        "Missing storage file reports an empty job list"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "         CLI Inspection Tests           \n";
    std::cout << "========================================\n";

    try {
        testListAllJobs();
        testFilteredListAndLimit();
        testStatusForExistingJob();
        testUnknownAndInvalidStatusQueries();
        testInvalidListOptions();
        testMissingStorageFile();
    } catch (const std::exception& exception) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected exception: "
            << exception.what()
            << '\n';
    } catch (...) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected non-standard exception\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';

    if (testsFailed == 0) {
        std::cout << "Result: ALL TESTS PASSED\n";
    } else {
        std::cout << "Result: TESTS FAILED\n";
    }

    std::cout << "========================================\n";

    resetStorage();

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
