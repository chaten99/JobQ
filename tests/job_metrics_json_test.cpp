#include "metrics/JobMetricsJson.h"
#include "cli/CommandHandlers.h"
#include "cli/CommandLine.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

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

bool hasNoTrailingCommas(const std::string& json) {
    // Check that there is no comma immediately followed by whitespace then '}' or ']'
    bool sawComma = false;
    for (char c : json) {
        if (c == ',') {
            sawComma = true;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            // whitespace, keep sawComma flag
        } else {
            if (sawComma && (c == '}' || c == ']')) {
                return false;
            }
            sawComma = false;
        }
    }
    return !sawComma;
}

bool hasBalancedBraces(const std::string& json) {
    int braceDepth = 0;
    int bracketDepth = 0;
    for (char c : json) {
        if (c == '{') ++braceDepth;
        else if (c == '}') --braceDepth;
        else if (c == '[') ++bracketDepth;
        else if (c == ']') --bracketDepth;

        if (braceDepth < 0 || bracketDepth < 0) return false;
    }
    return braceDepth == 0 && bracketDepth == 0;
}

// ============================================================================
// Test A — Empty Snapshot Serialization
// ============================================================================
void testEmptySnapshot() {
    std::cout << "\n--- Test A: Empty Snapshot Serialization ---\n";

    jobq::JobMetricsSnapshot snapshot;
    const std::string json = jobq::toJson(snapshot);

    check(
        json.find("\"schemaVersion\": 1") != std::string::npos,
        "JSON contains schemaVersion: 1"
    );
    check(
        json.find("\"metrics\": {") != std::string::npos,
        "JSON contains metrics object"
    );
    check(
        json.find("\"submittedJobs\": 0") != std::string::npos,
        "Empty snapshot has submittedJobs: 0"
    );
    check(
        json.find("\"startedExecutions\": 0") != std::string::npos,
        "Empty snapshot has startedExecutions: 0"
    );
    check(
        json.find("\"completedExecutions\": 0") != std::string::npos,
        "Empty snapshot has completedExecutions: 0"
    );
    check(
        json.find("\"failedExecutions\": 0") != std::string::npos,
        "Empty snapshot has failedExecutions: 0"
    );
    check(
        json.find("\"scheduledRetries\": 0") != std::string::npos,
        "Empty snapshot has scheduledRetries: 0"
    );
    check(
        json.find("\"finishedExecutions\": 0") != std::string::npos,
        "Empty snapshot has finishedExecutions: 0"
    );
    check(
        json.find("\"performance\": {") != std::string::npos,
        "JSON contains performance object"
    );
    check(
        json.find("\"totalExecutionTimeNs\": 0") != std::string::npos,
        "Empty snapshot has totalExecutionTimeNs: 0"
    );
    check(
        json.find("\"averageExecutionTimeNs\": 0") != std::string::npos,
        "Empty snapshot has averageExecutionTimeNs: 0"
    );
    check(
        json.find("\"elapsedTimeNs\": 0") != std::string::npos,
        "Empty snapshot has elapsedTimeNs: 0"
    );
    check(
        json.find("\"throughputJobsPerSecond\": 0.0") != std::string::npos,
        "Empty snapshot has throughputJobsPerSecond: 0.0"
    );
    check(
        hasNoTrailingCommas(json),
        "JSON output has no trailing commas"
    );
    check(
        hasBalancedBraces(json),
        "JSON output has balanced braces"
    );
}

// ============================================================================
// Test B — Populated Snapshot Serialization
// ============================================================================
void testPopulatedSnapshot() {
    std::cout << "\n--- Test B: Populated Snapshot Serialization ---\n";

    jobq::JobMetricsSnapshot snapshot;
    snapshot.submittedJobs = 100;
    snapshot.startedExecutions = 120;
    snapshot.completedExecutions = 90;
    snapshot.failedExecutions = 30;
    snapshot.scheduledRetries = 20;
    snapshot.finishedExecutions = 120;
    snapshot.totalExecutionTime = 2500000000ns;
    snapshot.averageExecutionTime = 20833333ns;
    snapshot.elapsedTime = 5000000000ns;
    snapshot.throughputJobsPerSecond = 24.0;

    const std::string json = jobq::toJson(snapshot);

    check(
        json.find("\"submittedJobs\": 100") != std::string::npos,
        "Populated snapshot serializes submittedJobs: 100"
    );
    check(
        json.find("\"startedExecutions\": 120") != std::string::npos,
        "Populated snapshot serializes startedExecutions: 120"
    );
    check(
        json.find("\"completedExecutions\": 90") != std::string::npos,
        "Populated snapshot serializes completedExecutions: 90"
    );
    check(
        json.find("\"failedExecutions\": 30") != std::string::npos,
        "Populated snapshot serializes failedExecutions: 30"
    );
    check(
        json.find("\"scheduledRetries\": 20") != std::string::npos,
        "Populated snapshot serializes scheduledRetries: 20"
    );
    check(
        json.find("\"finishedExecutions\": 120") != std::string::npos,
        "Populated snapshot serializes finishedExecutions: 120"
    );
    check(
        json.find("\"totalExecutionTimeNs\": 2500000000") != std::string::npos,
        "Populated snapshot serializes totalExecutionTimeNs: 2500000000"
    );
    check(
        json.find("\"averageExecutionTimeNs\": 20833333") != std::string::npos,
        "Populated snapshot serializes averageExecutionTimeNs: 20833333"
    );
    check(
        json.find("\"elapsedTimeNs\": 5000000000") != std::string::npos,
        "Populated snapshot serializes elapsedTimeNs: 5000000000"
    );
    check(
        json.find("\"throughputJobsPerSecond\": 24.0") != std::string::npos,
        "Populated snapshot serializes throughputJobsPerSecond: 24.0"
    );
    check(
        hasNoTrailingCommas(json),
        "JSON output has no trailing commas"
    );
    check(
        hasBalancedBraces(json),
        "JSON output has balanced braces"
    );
}

// ============================================================================
// Test C — Duration Precision in Nanoseconds
// ============================================================================
void testDurationPrecision() {
    std::cout << "\n--- Test C: Duration Precision in Nanoseconds ---\n";

    jobq::JobMetricsSnapshot snapshot;
    snapshot.totalExecutionTime = 123456789ns;
    snapshot.averageExecutionTime = 987654321ns;
    snapshot.elapsedTime = 999888777666ns;

    const std::string json = jobq::toJson(snapshot);

    check(
        json.find("\"totalExecutionTimeNs\": 123456789") != std::string::npos,
        "Exact nanoseconds preserved for totalExecutionTimeNs"
    );
    check(
        json.find("\"averageExecutionTimeNs\": 987654321") != std::string::npos,
        "Exact nanoseconds preserved for averageExecutionTimeNs"
    );
    check(
        json.find("\"elapsedTimeNs\": 999888777666") != std::string::npos,
        "Exact nanoseconds preserved for elapsedTimeNs"
    );
}

// ============================================================================
// Test D — Floating-Point Throughput Serialization
// ============================================================================
void testFloatingPointSerialization() {
    std::cout << "\n--- Test D: Floating-Point Throughput Serialization ---\n";

    // 1. Positive throughput with decimals
    {
        jobq::JobMetricsSnapshot snapshot;
        snapshot.throughputJobsPerSecond = 123.456;
        const std::string json = jobq::toJson(snapshot);
        check(
            json.find("\"throughputJobsPerSecond\": 123.456") != std::string::npos,
            "Finite decimal throughput serialized accurately"
        );
    }

    // 2. Large throughput
    {
        jobq::JobMetricsSnapshot snapshot;
        snapshot.throughputJobsPerSecond = 5000000.0;
        const std::string json = jobq::toJson(snapshot);
        check(
            json.find("\"throughputJobsPerSecond\": 5000000.0") != std::string::npos,
            "Large finite throughput serialized as valid number"
        );
    }

    // 3. Small positive throughput
    {
        jobq::JobMetricsSnapshot snapshot;
        snapshot.throughputJobsPerSecond = 0.005;
        const std::string json = jobq::toJson(snapshot);
        check(
            json.find("\"throughputJobsPerSecond\": 0.005") != std::string::npos,
            "Small positive throughput serialized correctly"
        );
    }

    // 4. Non-finite: NaN
    {
        jobq::JobMetricsSnapshot snapshot;
        snapshot.throughputJobsPerSecond =
            std::numeric_limits<double>::quiet_NaN();
        const std::string json = jobq::toJson(snapshot);
        check(
            json.find("\"throughputJobsPerSecond\": null") != std::string::npos,
            "NaN throughput safely serialized as JSON null"
        );
        check(
            json.find("nan") == std::string::npos &&
                json.find("NaN") == std::string::npos,
            "Output does not contain invalid NaN token"
        );
    }

    // 5. Non-finite: +Infinity
    {
        jobq::JobMetricsSnapshot snapshot;
        snapshot.throughputJobsPerSecond =
            std::numeric_limits<double>::infinity();
        const std::string json = jobq::toJson(snapshot);
        check(
            json.find("\"throughputJobsPerSecond\": null") != std::string::npos,
            "+Infinity throughput safely serialized as JSON null"
        );
        check(
            json.find("inf") == std::string::npos &&
                json.find("Infinity") == std::string::npos,
            "Output does not contain invalid Infinity token"
        );
    }

    // 6. Non-finite: -Infinity
    {
        jobq::JobMetricsSnapshot snapshot;
        snapshot.throughputJobsPerSecond =
            -std::numeric_limits<double>::infinity();
        const std::string json = jobq::toJson(snapshot);
        check(
            json.find("\"throughputJobsPerSecond\": null") != std::string::npos,
            "-Infinity throughput safely serialized as JSON null"
        );
    }
}

// ============================================================================
// Test E — Output Structure & Determinism
// ============================================================================
void testOutputStructureAndDeterminism() {
    std::cout << "\n--- Test E: Output Structure & Determinism ---\n";

    jobq::JobMetricsSnapshot snapshot;
    snapshot.submittedJobs = 42;
    snapshot.startedExecutions = 42;
    snapshot.completedExecutions = 40;
    snapshot.failedExecutions = 2;
    snapshot.scheduledRetries = 0;
    snapshot.finishedExecutions = 42;
    snapshot.totalExecutionTime = 42000000ns;
    snapshot.averageExecutionTime = 1000000ns;
    snapshot.elapsedTime = 50000000ns;
    snapshot.throughputJobsPerSecond = 840.0;

    const std::string first = jobq::toJson(snapshot);
    const std::string second = jobq::toJson(snapshot);

    check(
        first == second,
        "toJson is completely deterministic across repeated calls"
    );

    check(
        first.front() == '{' && first.back() == '}',
        "JSON starts with '{' and ends with '}' without extra decoration"
    );

    check(
        hasNoTrailingCommas(first),
        "Deterministic JSON contains no trailing commas"
    );
}

// ============================================================================
// Test F — Existing CLI Integration & Format Flag
// ============================================================================
void testCliIntegration() {
    std::cout << "\n--- Test F: CLI Integration with --format ---\n";

    jobq::JobMetrics liveMetrics;
    liveMetrics.recordJobSubmitted();
    liveMetrics.recordExecutionStarted();
    liveMetrics.recordExecutionCompleted(10ms);

    // 1. metrics --format json with live metrics
    {
        jobq::cli::ParsedCommandLine parsed;
        parsed.command = "metrics";
        parsed.options["format"] = "json";

        std::ostringstream output;
        std::ostringstream errorOutput;

        const int result = jobq::cli::executeMetricsCommand(
            parsed,
            output,
            errorOutput,
            &liveMetrics
        );

        check(
            result == jobq::cli::kSuccessExitCode,
            "executeMetricsCommand succeeds with --format json"
        );
        check(
            output.str().find("\"schemaVersion\": 1") != std::string::npos,
            "Metrics JSON output includes schemaVersion"
        );
        check(
            output.str().find("\"submittedJobs\": 1") != std::string::npos,
            "Metrics JSON output reflects live metrics count"
        );
        check(
            output.str().find("========== JobQ Runtime Metrics ==========") ==
                std::string::npos,
            "Metrics JSON output does not include human-readable header banner"
        );
        check(
            errorOutput.str().empty(),
            "Error output is empty on successful JSON export"
        );
    }

    // 2. metrics --format text (default backward-compatibility)
    {
        jobq::cli::ParsedCommandLine parsed;
        parsed.command = "metrics";
        parsed.options["format"] = "text";

        std::ostringstream output;
        std::ostringstream errorOutput;

        const int result = jobq::cli::executeMetricsCommand(
            parsed,
            output,
            errorOutput,
            &liveMetrics
        );

        check(
            result == jobq::cli::kSuccessExitCode,
            "executeMetricsCommand succeeds with --format text"
        );
        check(
            output.str().find("========== JobQ Runtime Metrics ==========") !=
                std::string::npos,
            "Text format retains human-readable header banner"
        );
        check(
            output.str().find("Submitted Jobs:          1") != std::string::npos,
            "Text format outputs human-readable metric labels"
        );
    }

    // 3. metrics --format invalid produces usage error
    {
        jobq::cli::ParsedCommandLine parsed;
        parsed.command = "metrics";
        parsed.options["format"] = "yaml";

        std::ostringstream output;
        std::ostringstream errorOutput;

        const int result = jobq::cli::executeMetricsCommand(
            parsed,
            output,
            errorOutput,
            &liveMetrics
        );

        check(
            result == jobq::cli::kUsageErrorExitCode,
            "executeMetricsCommand rejects invalid format argument"
        );
        check(
            errorOutput.str().find("Invalid format") != std::string::npos,
            "Error output explains that format must be text or json"
        );
    }

    // 4. Command line parser recognizes --format for metrics and run
    {
        jobq::cli::ParsedCommandLine parsed;
        std::string parseError;

        check(
            jobq::cli::parseCommandLine(
                {"metrics", "--format", "json"},
                parsed,
                parseError
            ) && parsed.options["format"] == "json",
            "CommandLine parser accepts --format for metrics command"
        );

        check(
            jobq::cli::parseCommandLine(
                {"run", "--format", "json"},
                parsed,
                parseError
            ) && parsed.options["format"] == "json",
            "CommandLine parser accepts --format for run command"
        );
    }

    // 5. executeRunCommand with --format json
    {
        const auto testDir =
            std::filesystem::temp_directory_path() / "jobq_cli_run_json_test";
        const auto storagePath = testDir / "jobs.journal";

        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::create_directories(testDir, ec);

        auto storage = std::make_shared<jobq::FileJobStorage>(storagePath);
        jobq::JobRecord record;
        record.id = 1;
        record.type = "REPORT";
        record.payload = "json-export-run";
        record.status = jobq::JobStatus::Pending;
        record.attemptsMade = 0;
        storage->save(record);

        jobq::cli::ParsedCommandLine parsed;
        parsed.command = "run";
        parsed.options["storage"] = storagePath.string();
        parsed.options["workers"] = "2";
        parsed.options["format"] = "json";

        std::ostringstream output;
        std::ostringstream errorOutput;

        const int result = jobq::cli::executeRunCommand(
            parsed,
            output,
            errorOutput
        );

        check(
            result == jobq::cli::kSuccessExitCode,
            "executeRunCommand succeeds with --format json"
        );
        check(
            output.str().find("\"schemaVersion\": 1") != std::string::npos,
            "Run output contains JSON schemaVersion block"
        );
        check(
            output.str().find("\"completedExecutions\": 1") != std::string::npos,
            "Run output JSON contains completedExecutions: 1"
        );

        std::filesystem::remove_all(testDir, ec);
    }
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "      JobQ Runtime Metrics JSON Tests\n";
    std::cout << "========================================\n";

    try {
        testEmptySnapshot();
        testPopulatedSnapshot();
        testDurationPrecision();
        testFloatingPointSerialization();
        testOutputStructureAndDeterminism();
        testCliIntegration();
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

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
