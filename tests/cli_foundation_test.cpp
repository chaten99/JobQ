
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cli/CommandLine.h"

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

void testHelp() {
    std::cout << "\n--- Help Handling ---\n";

    jobq::cli::ParsedCommandLine parsed;
    std::string error;

    check(
        jobq::cli::parseCommandLine(
            {"--help"},
            parsed,
            error
        ) && parsed.helpRequested,
        "--help works without a command"
    );

    check(
        jobq::cli::parseCommandLine(
            {"submit", "-h"},
            parsed,
            error
        ) && parsed.helpRequested,
        "-h works after a command"
    );

    check(
        jobq::cli::parseCommandLine(
            {"unknown-command", "--help"},
            parsed,
            error
        ) && parsed.helpRequested,
        "Help is available even with an invalid command"
    );

    const auto usage = jobq::cli::usageText();

    check(
        usage.find("Usage:") != std::string::npos &&
            usage.find("submit") != std::string::npos &&
            usage.find("status") != std::string::npos,
        "Usage text lists CLI commands"
    );
}

void testCommandRecognition() {
    std::cout << "\n--- Command Recognition ---\n";

    jobq::cli::ParsedCommandLine parsed;
    std::string error;

    check(
        jobq::cli::parseCommandLine({"submit"}, parsed, error) &&
            parsed.command == "submit",
        "submit command is recognized"
    );

    check(
        jobq::cli::parseCommandLine({"list"}, parsed, error) &&
            parsed.command == "list",
        "list command is recognized"
    );

    check(
        jobq::cli::parseCommandLine(
            {"status", "42"},
            parsed,
            error
        ) &&
            parsed.command == "status" &&
            parsed.positionalArguments.size() == 1 &&
            parsed.positionalArguments[0] == "42",
        "status command accepts a positional job ID"
    );

    check(
        jobq::cli::parseCommandLine({"run"}, parsed, error) &&
            parsed.command == "run",
        "run command is recognized"
    );
}

void testOptions() {
    std::cout << "\n--- Option Parsing ---\n";

    jobq::cli::ParsedCommandLine parsed;
    std::string error;

    const bool parsedSubmit = jobq::cli::parseCommandLine(
        {
            "submit",
            "--type=EMAIL",
            "--payload",
            "Welcome email",
            "--priority",
            "high",
            "--retries",
            "3",
            "--storage",
            "test/jobs.journal"
        },
        parsed,
        error
    );

    check(
        parsedSubmit,
        "Options support --key=value and --key value"
    );

    check(
        parsed.options["type"] == "EMAIL",
        "Type option is parsed correctly"
    );

    check(
        parsed.options["payload"] == "Welcome email",
        "Payload with spaces is preserved"
    );

    check(
        parsed.options["priority"] == "high" &&
            parsed.options["retries"] == "3",
        "Priority and retries options are parsed"
    );

    check(
        parsed.options["storage"] == "test/jobs.journal",
        "Global storage option is parsed"
    );

    check(
        jobq::cli::parseCommandLine(
            {"run", "--workers=4"},
            parsed,
            error
        ) &&
            parsed.options["workers"] == "4",
        "Workers option supports equals syntax"
    );

    check(
        jobq::cli::parseCommandLine(
            {"list", "--status", "FAILED", "--limit", "20"},
            parsed,
            error
        ) &&
            parsed.options["status"] == "FAILED" &&
            parsed.options["limit"] == "20",
        "List options are parsed correctly"
    );
}

void testInvalidArguments() {
    std::cout << "\n--- Invalid Argument Handling ---\n";

    jobq::cli::ParsedCommandLine parsed;
    std::string error;

    check(
        !jobq::cli::parseCommandLine({}, parsed, error) &&
            !error.empty(),
        "Missing command returns an error"
    );

    check(
        !jobq::cli::parseCommandLine(
            {"unknown"},
            parsed,
            error
        ) &&
            error.find("Unknown command") != std::string::npos,
        "Unknown command is rejected"
    );

    check(
        !jobq::cli::parseCommandLine(
            {"submit", "--type"},
            parsed,
            error
        ) &&
            error.find("Missing value") != std::string::npos,
        "Missing option value is rejected"
    );

    check(
        !jobq::cli::parseCommandLine(
            {"submit", "--unknown", "value"},
            parsed,
            error
        ),
        "Unknown command-specific option is rejected"
    );

    check(
        !jobq::cli::parseCommandLine(
            {"submit", "--type", "EMAIL", "--type", "REPORT"},
            parsed,
            error
        ) &&
            error.find("Duplicate option") != std::string::npos,
        "Duplicate options are rejected"
    );

    check(
        !jobq::cli::parseCommandLine(
            {"list", "-x"},
            parsed,
            error
        ),
        "Unsupported short option is rejected"
    );

    check(
        !jobq::cli::parseCommandLine(
            {"submit", "--=EMAIL"},
            parsed,
            error
        ),
        "Empty option name is rejected"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "         CLI Foundation Tests           \n";
    std::cout << "========================================\n";

    try {
        testHelp();
        testCommandRecognition();
        testOptions();
        testInvalidArguments();
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

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
