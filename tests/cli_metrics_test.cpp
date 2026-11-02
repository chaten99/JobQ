#include "cli/CommandHandlers.h"
#include "cli/CommandLine.h"
#include "metrics/JobMetrics.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"

#include <chrono>
#include <filesystem>
#include <iostream>
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

std::filesystem::path createTestDirectory(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void cleanupDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testDurationFormatting() {
    std::cout << "\n--- Duration Formatting Tests ---\n";

    check(
        jobq::cli::formatDuration(0ns) == "0 ns",
        "Zero nanoseconds formats as '0 ns'"
    );
    check(
        jobq::cli::formatDuration(-5ns) == "0 ns",
        "Negative nanoseconds formats safely as '0 ns'"
    );
    check(
        jobq::cli::formatDuration(750ns) == "750 ns",
        "Sub-microsecond duration formats as nanoseconds"
    );
    check(
        jobq::cli::formatDuration(1500ns) == "1.50 us",
        "Sub-millisecond duration formats as microseconds with 2 decimal places"
    );
    check(
        jobq::cli::formatDuration(2500000ns) == "2.50 ms",
        "Sub-second duration formats as milliseconds with 2 decimal places"
    );
    check(
        jobq::cli::formatDuration(1250000000ns) == "1.25 s",
        "Multi-second duration formats as seconds with 2 decimal places"
    );
}

void testThroughputFormatting() {
    std::cout << "\n--- Throughput Formatting Tests ---\n";

    check(
        jobq::cli::formatThroughput(0.0) == "0.00 jobs/sec",
        "Zero throughput formats as '0.00 jobs/sec'"
    );
    check(
        jobq::cli::formatThroughput(123.456) == "123.46 jobs/sec",
        "Throughput formats with 2 decimal places and 'jobs/sec' suffix"
    );
}

void testEmptyMetricsFormatting() {
    std::cout << "\n--- Empty Metrics Formatting Tests ---\n";

    jobq::JobMetricsSnapshot emptySnapshot;
    const std::string formatted = jobq::cli::formatMetrics(emptySnapshot);

    check(
        formatted.find("========== JobQ Runtime Metrics ==========") != std::string::npos,
        "Report contains the main header"
    );
    check(
        formatted.find("Job Statistics") != std::string::npos,
        "Report contains Job Statistics section"
    );
    check(
        formatted.find("Performance Statistics") != std::string::npos,
        "Report contains Performance Statistics section"
    );
    check(
        formatted.find("  Submitted Jobs:          0") != std::string::npos,
        "Empty snapshot has 0 submitted jobs"
    );
    check(
        formatted.find("  Started Executions:      0") != std::string::npos,
        "Empty snapshot has 0 started executions"
    );
    check(
        formatted.find("  Completed Executions:    0") != std::string::npos,
        "Empty snapshot has 0 completed executions"
    );
    check(
        formatted.find("  Failed Executions:       0") != std::string::npos,
        "Empty snapshot has 0 failed executions"
    );
    check(
        formatted.find("  Scheduled Retries:       0") != std::string::npos,
        "Empty snapshot has 0 scheduled retries"
    );
    check(
        formatted.find("  Finished Executions:     0") != std::string::npos,
        "Empty snapshot has 0 finished executions"
    );
    check(
        formatted.find("  Total Execution Time:    0 ns") != std::string::npos,
        "Empty snapshot has '0 ns' total execution time"
    );
    check(
        formatted.find("  Average Execution Time:  0 ns") != std::string::npos,
        "Empty snapshot has '0 ns' average execution time"
    );
    check(
        formatted.find("  Throughput:              0.00 jobs/sec") != std::string::npos,
        "Empty snapshot has '0.00 jobs/sec' throughput"
    );
    check(
        formatted.find("==========================================") != std::string::npos,
        "Report contains the closing footer"
    );

    std::ostringstream out;
    jobq::cli::printMetrics(emptySnapshot, out);
    check(
        out.str().ends_with("==========================================\n"),
        "printMetrics appends a final newline"
    );
}

void testCommandParsing() {
    std::cout << "\n--- Command Line Parsing for Metrics ---\n";

    jobq::cli::ParsedCommandLine parsed;
    std::string error;

    check(
        jobq::cli::parseCommandLine({"metrics"}, parsed, error) &&
            parsed.command == "metrics",
        "parseCommandLine recognizes 'metrics' command"
    );

    check(
        jobq::cli::parseCommandLine({"metrics", "--help"}, parsed, error) &&
            parsed.helpRequested,
        "'metrics --help' requests help"
    );

    check(
        !jobq::cli::parseCommandLine({"metrics", "--unknown", "val"}, parsed, error) &&
            error.find("Unknown option") != std::string::npos,
        "parseCommandLine rejects unknown options for 'metrics'"
    );

    const auto usage = jobq::cli::usageText();
    check(
        usage.find("metrics") != std::string::npos,
        "Usage text mentions the metrics command"
    );
}

void testStandaloneMetricsHandling() {
    std::cout << "\n--- Standalone Metrics Handling ---\n";

    jobq::cli::ParsedCommandLine parsed;
    parsed.command = "metrics";

    std::ostringstream output;
    std::ostringstream errorOutput;

    const int exitCode = jobq::cli::executeMetricsCommand(
        parsed,
        output,
        errorOutput,
        nullptr
    );

    check(
        exitCode == jobq::cli::kCommandErrorExitCode,
        "Standalone metrics command without live metrics returns command error code"
    );
    check(
        errorOutput.str().find("in memory") != std::string::npos,
        "Error output explains that metrics are in-memory"
    );
    check(
        errorOutput.str().find("'run'") != std::string::npos,
        "Error output guides user to run command"
    );
    check(
        output.str().empty(),
        "Standard output is empty when standalone metrics command is rejected"
    );

    // Reject positional arguments
    parsed.positionalArguments.push_back("extra");
    std::ostringstream out2;
    std::ostringstream err2;
    const int exitCode2 = jobq::cli::executeMetricsCommand(
        parsed,
        out2,
        err2,
        nullptr
    );
    check(
        exitCode2 == jobq::cli::kUsageErrorExitCode,
        "Metrics command rejects positional arguments"
    );

    // Reject wrong command
    parsed.command = "wrong";
    std::ostringstream out3;
    std::ostringstream err3;
    const int exitCode3 = jobq::cli::executeMetricsCommand(
        parsed,
        out3,
        err3,
        nullptr
    );
    check(
        exitCode3 == jobq::cli::kUsageErrorExitCode,
        "Metrics handler rejects mismatched command"
    );
}

void testLiveMetricsReportingViaHandler() {
    std::cout << "\n--- Live Metrics Reporting via Handler ---\n";

    jobq::JobMetrics liveMetrics;
    liveMetrics.recordJobSubmitted();
    liveMetrics.recordExecutionStarted();
    liveMetrics.recordExecutionCompleted(15ms);

    jobq::cli::ParsedCommandLine parsed;
    parsed.command = "metrics";

    std::ostringstream output;
    std::ostringstream errorOutput;

    const int exitCode = jobq::cli::executeMetricsCommand(
        parsed,
        output,
        errorOutput,
        &liveMetrics
    );

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "executeMetricsCommand succeeds when provided with a live JobMetrics pointer"
    );
    check(
        output.str().find("Submitted Jobs:          1") != std::string::npos,
        "Handler output reflects submitted jobs from live metrics"
    );
    check(
        output.str().find("Completed Executions:    1") != std::string::npos,
        "Handler output reflects completed executions from live metrics"
    );
    check(
        output.str().find("Finished Executions:     1") != std::string::npos,
        "Handler output reflects finished executions from live metrics"
    );
    check(
        output.str().find("15.00 ms") != std::string::npos,
        "Handler output reflects recorded execution duration"
    );
    check(
        errorOutput.str().empty(),
        "Error output is empty on successful metrics report"
    );
}

void testRunCommandReportsMetricsSuccessfulJobs() {
    std::cout << "\n--- Run Command Reports Metrics for Successful Jobs ---\n";

    const auto testDir = createTestDirectory("jobq_cli_metrics_success");
    const auto storagePath = testDir / "jobs.journal";

    auto storage = std::make_shared<jobq::FileJobStorage>(storagePath);
    {
        jobq::JobRecord r1;
        r1.id = 1;
        r1.type = "REPORT";
        r1.payload = "annual-audit";
        r1.priority = jobq::JobPriority::Normal;
        r1.status = jobq::JobStatus::Pending;
        r1.attemptsMade = 0;
        storage->save(r1);

        jobq::JobRecord r2;
        r2.id = 2;
        r2.type = "EMAIL";
        r2.payload = "welcome-user";
        r2.priority = jobq::JobPriority::High;
        r2.status = jobq::JobStatus::Pending;
        r2.attemptsMade = 0;
        storage->save(r2);
    }

    jobq::cli::ParsedCommandLine parsed;
    parsed.command = "run";
    parsed.options["storage"] = storagePath.string();
    parsed.options["workers"] = "2";

    std::ostringstream output;
    std::ostringstream errorOutput;

    const int exitCode = jobq::cli::executeRunCommand(
        parsed,
        output,
        errorOutput
    );

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "executeRunCommand succeeds"
    );

    const std::string text = output.str();

    check(
        text.find("JobQ stopped cleanly.") != std::string::npos,
        "Run output contains 'JobQ stopped cleanly.'"
    );
    check(
        text.find("Completed jobs: 2") != std::string::npos,
        "Run output contains 'Completed jobs: 2'"
    );
    check(
        text.find("========== JobQ Runtime Metrics ==========") != std::string::npos,
        "Run output includes the runtime metrics block"
    );
    check(
        text.find("Completed Executions:    2") != std::string::npos,
        "Runtime metrics block reports 2 completed executions"
    );
    check(
        text.find("Failed Executions:       0") != std::string::npos,
        "Runtime metrics block reports 0 failed executions"
    );
    check(
        text.find("Finished Executions:     2") != std::string::npos,
        "Runtime metrics block reports 2 finished executions"
    );
    check(
        text.find("jobs/sec") != std::string::npos,
        "Runtime metrics block includes throughput calculation"
    );

    cleanupDirectory(testDir);
}

void testRunCommandReportsMetricsFailedJobsAndRetries() {
    std::cout << "\n--- Run Command Reports Metrics for Failed Jobs & Retries ---\n";

    const auto testDir = createTestDirectory("jobq_cli_metrics_fail_retry");
    const auto storagePath = testDir / "jobs.journal";

    auto storage = std::make_shared<jobq::FileJobStorage>(storagePath);
    {
        // Job with maxRetries = 1, will fail once and schedule a retry
        jobq::JobRecord r1;
        r1.id = 1;
        r1.type = "FAIL";
        r1.payload = "retryable-failure";
        r1.priority = jobq::JobPriority::Normal;
        r1.status = jobq::JobStatus::Pending;
        r1.attemptsMade = 0;
        r1.retryPolicy.maxRetries = 1;
        r1.retryPolicy.initialDelay = 10ms;
        storage->save(r1);
    }

    jobq::cli::ParsedCommandLine parsed;
    parsed.command = "run";
    parsed.options["storage"] = storagePath.string();
    parsed.options["workers"] = "2";

    std::ostringstream output;
    std::ostringstream errorOutput;

    const int exitCode = jobq::cli::executeRunCommand(
        parsed,
        output,
        errorOutput
    );

    check(
        exitCode == jobq::cli::kSuccessExitCode,
        "executeRunCommand succeeds even with job failures"
    );

    const std::string text = output.str();

    check(
        text.find("========== JobQ Runtime Metrics ==========") != std::string::npos,
        "Run output includes runtime metrics block"
    );
    check(
        text.find("Failed Executions:       2") != std::string::npos,
        "Runtime metrics reports both failed attempts (initial + retry)"
    );
    check(
        text.find("Scheduled Retries:       1") != std::string::npos,
        "Runtime metrics reports 1 scheduled retry"
    );
    check(
        text.find("Finished Executions:     2") != std::string::npos,
        "Runtime metrics reports 2 finished executions"
    );

    cleanupDirectory(testDir);
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "      JobQ CLI Metrics Tests\n";
    std::cout << "========================================\n";

    try {
        testDurationFormatting();
        testThroughputFormatting();
        testEmptyMetricsFormatting();
        testCommandParsing();
        testStandaloneMetricsHandling();
        testLiveMetricsReportingViaHandler();
        testRunCommandReportsMetricsSuccessfulJobs();
        testRunCommandReportsMetricsFailedJobsAndRetries();
    } catch (const std::exception& ex) {
        ++testsFailed;
        std::cerr << "[FAIL] Unexpected exception: " << ex.what() << '\n';
    } catch (...) {
        ++testsFailed;
        std::cerr << "[FAIL] Unexpected non-standard exception\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0
        ? "Result: ALL TESTS PASSED\n"
        : "Result: TEST FAILURES DETECTED\n");
    std::cout << "========================================\n";

    return testsFailed == 0 ? 0 : 1;
}
