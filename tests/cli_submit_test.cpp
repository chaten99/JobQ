
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/CommandHandlers.h"
#include "cli/CommandLine.h"
#include "storage/FileJobStorage.h"

namespace {

int testsRun = 0;
int testsFailed = 0;

const std::filesystem::path& testRoot() {
    static const auto root =
        std::filesystem::temp_directory_path() /
        "jobq_cli_submit_tests";

    return root;
}

const std::filesystem::path& journalPath() {
    static const auto path =
        testRoot() / "jobs.journal";

    return path;
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

int executeArgs(const std::vector<std::string>& arguments,
                std::ostringstream& output,
                std::ostringstream& error) {
    jobq::cli::ParsedCommandLine parsed;
    std::string parseError;

    if (!jobq::cli::parseCommandLine(
            arguments,
            parsed,
            parseError
        )) {
        error << parseError << '\n';
        return jobq::cli::kUsageErrorExitCode;
    }

    if (parsed.helpRequested) {
        output << jobq::cli::usageText();
        return jobq::cli::kSuccessExitCode;
    }

    if (parsed.command == "submit") {
        return jobq::cli::executeSubmitCommand(
            parsed,
            output,
            error
        );
    }

    return jobq::cli::kNotImplementedExitCode;
}

std::vector<std::string> validArguments(
    const std::filesystem::path& path,
    const std::string& payload
) {
    return {
        "submit",
        "--type", "EMAIL",
        "--payload", payload,
        "--priority", "high",
        "--retries", "3",
        "--storage", path.string()
    };
}

void testSuccessfulSubmission() {
    std::cout << "\n--- Successful Job Submission ---\n";

    resetStorage();

    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = executeArgs(
        validArguments(journalPath(), "Welcome email"),
        output,
        error
    );

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "Valid submit command succeeds"
    );

    check(
        output.str().find("Job submitted successfully") !=
            std::string::npos,
        "Success message is displayed"
    );

    check(
        output.str().find("ID: 1") != std::string::npos,
        "Generated job ID is displayed"
    );

    check(
        output.str().find("Priority: HIGH") != std::string::npos,
        "Selected priority is displayed"
    );

    check(
        output.str().find("Maximum retries: 3") !=
            std::string::npos,
        "Retry count is displayed"
    );

    jobq::FileJobStorage storage(journalPath());
    const auto records = storage.loadAll();

    check(
        records.size() == 1,
        "Submitted job is persisted"
    );

    check(
        records.size() == 1 &&
            records.front().id == 1 &&
            records.front().type == "EMAIL" &&
            records.front().payload == "Welcome email",
        "Persisted record contains the expected job data"
    );

    check(
        records.size() == 1 &&
            records.front().priority == jobq::JobPriority::High &&
            records.front().retryPolicy.maxRetries == 3 &&
            records.front().status == jobq::JobStatus::Pending,
        "Persisted priority, retry policy, and status are correct"
    );

    check(
        error.str().empty(),
        "Successful submission emits no error output"
    );
}

void testSubmissionIdContinuesAcrossInvocations() {
    std::cout << "\n--- Persistent ID Continuity ---\n";

    resetStorage();

    std::ostringstream firstOutput;
    std::ostringstream firstError;

    const int firstExitCode = executeArgs(
        validArguments(journalPath(), "first"),
        firstOutput,
        firstError
    );

    std::ostringstream secondOutput;
    std::ostringstream secondError;

    const int secondExitCode = executeArgs(
        validArguments(journalPath(), "second"),
        secondOutput,
        secondError
    );

    check(
        firstExitCode == 0 && secondExitCode == 0,
        "Multiple submit invocations succeed"
    );

    check(
        secondOutput.str().find("ID: 2") != std::string::npos,
        "Second CLI invocation continues the persisted ID sequence"
    );

    jobq::FileJobStorage storage(journalPath());

    check(
        storage.loadAll().size() == 2,
        "Both submitted jobs survive separate invocations"
    );
}

void testInvalidSubmissionOptions() {
    std::cout << "\n--- Submission Validation ---\n";

    resetStorage();

    std::ostringstream output;
    std::ostringstream error;

    auto arguments = validArguments(
        journalPath(),
        "missing type"
    );

    arguments.erase(arguments.begin() + 1, arguments.begin() + 3);

    const int missingTypeExitCode = executeArgs(
        arguments,
        output,
        error
    );

    check(
        missingTypeExitCode == jobq::cli::kUsageErrorExitCode,
        "Missing type is rejected"
    );

    arguments = validArguments(
        journalPath(),
        "invalid priority"
    );

    const auto priorityIterator =
        std::find(arguments.begin(), arguments.end(), "high");

    if (priorityIterator != arguments.end()) {
        *priorityIterator = "urgent";
    }

    output.str("");
    output.clear();
    error.str("");
    error.clear();

    const int invalidPriorityExitCode = executeArgs(
        arguments,
        output,
        error
    );

    check(
        invalidPriorityExitCode ==
            jobq::cli::kUsageErrorExitCode,
        "Invalid priority is rejected"
    );

    arguments = validArguments(
        journalPath(),
        "invalid retries"
    );

    const auto retryIterator =
        std::find(arguments.begin(), arguments.end(), "3");

    if (retryIterator != arguments.end()) {
        *retryIterator = "-1";
    }

    output.str("");
    output.clear();
    error.str("");
    error.clear();

    const int invalidRetriesExitCode = executeArgs(
        arguments,
        output,
        error
    );

    check(
        invalidRetriesExitCode ==
            jobq::cli::kUsageErrorExitCode,
        "Negative retry count is rejected"
    );

    check(
        !std::filesystem::exists(journalPath()),
        "Invalid submission options do not create storage"
    );
}

void testStorageFailureIsReported() {
    std::cout << "\n--- Storage Failure ---\n";

    resetStorage();

    std::filesystem::create_directories(journalPath());

    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = executeArgs(
        validArguments(journalPath(), "cannot write"),
        output,
        error
    );

    check(
        exitCode == jobq::cli::kCommandErrorExitCode,
        "Storage errors return the command-error exit code"
    );

    check(
        error.str().find("Submission failed") !=
            std::string::npos,
        "Storage failure reports a readable error"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "           CLI Submit Tests             \n";
    std::cout << "========================================\n";

    try {
        testSuccessfulSubmission();
        testSubmissionIdContinuesAcrossInvocations();
        testInvalidSubmissionOptions();
        testStorageFailureIsReported();
    } catch (const std::exception& exception) {
        ++testsFailed;
        std::cerr << "[FAIL] Unexpected exception: "
                  << exception.what() << '\n';
    } catch (...) {
        ++testsFailed;
        std::cerr << "[FAIL] Unexpected non-standard exception\n";
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
