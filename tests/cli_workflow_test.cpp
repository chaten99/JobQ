#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

int testsRun = 0;
int testsFailed = 0;

struct ProcessResult {
    int exitCode{-1};
    std::string output;
};

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

std::string quoteArgument(const std::string& value) {
#ifdef _WIN32
    // Escape arguments according to Windows command-line quoting rules.
    std::string result = "\"";
    std::size_t backslashes = 0;

    for (const char character : value) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }

        if (character == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
        } else {
            result.append(backslashes, '\\');
            result.push_back(character);
        }

        backslashes = 0;
    }

    result.append(backslashes * 2, '\\');
    result.push_back('"');

    return result;
#else
    // Single-quote each argument for a POSIX shell.
    std::string result = "'";

    for (const char character : value) {
        if (character == '\'') {
            result += "'\\''";
        } else {
            result.push_back(character);
        }
    }

    result.push_back('\'');
    return result;
#endif
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);

    if (!input) {
        return {};
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

ProcessResult runCli(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& outputPath
) {
    std::string commandLine = quoteArgument(executable.string());

    for (const auto& argument : arguments) {
        commandLine += " ";
        commandLine += quoteArgument(argument);
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE outputHandle = CreateFileA(
        outputPath.string().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (outputHandle == INVALID_HANDLE_VALUE) {
        return {
            -1,
            "Unable to create process output file. Windows error: " +
                std::to_string(GetLastError())
        };
    }

    // Give child stdin a harmless NUL input handle. Both stdout and stderr
    // are directed into the same output file for diagnosis.
    HANDLE inputHandle = CreateFileA(
        "NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (inputHandle == INVALID_HANDLE_VALUE) {
        const DWORD errorCode = GetLastError();
        CloseHandle(outputHandle);

        return {
            -1,
            "Unable to open NUL input. Windows error: " +
                std::to_string(errorCode)
        };
    }

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = inputHandle;
    startupInfo.hStdOutput = outputHandle;
    startupInfo.hStdError = outputHandle;

    PROCESS_INFORMATION processInfo{};

    // CreateProcessA requires a mutable command-line buffer.
    std::vector<char> mutableCommandLine(
        commandLine.begin(),
        commandLine.end()
    );
    mutableCommandLine.push_back('\0');

    const BOOL created = CreateProcessA(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo
    );

    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

    CloseHandle(outputHandle);
    CloseHandle(inputHandle);

    if (!created) {
        return {
            -1,
            "CreateProcess failed. Windows error: " +
                std::to_string(createError) +
                "\nCommand: " + commandLine
        };
    }

    const DWORD waitResult = WaitForSingleObject(
        processInfo.hProcess,
        INFINITE
    );

    DWORD processExitCode = 1;
    bool exitCodeRead = false;

    if (waitResult == WAIT_OBJECT_0) {
        exitCodeRead = GetExitCodeProcess(
            processInfo.hProcess,
            &processExitCode
        ) != 0;
    }

    const DWORD waitError = GetLastError();

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (waitResult != WAIT_OBJECT_0) {
        return {
            -1,
            "Waiting for CLI process failed. Windows error: " +
                std::to_string(waitError)
        };
    }

    if (!exitCodeRead) {
        return {
            -1,
            "Unable to read CLI process exit code. Windows error: " +
                std::to_string(waitError)
        };
    }

    return {
        static_cast<int>(processExitCode),
        readFile(outputPath)
    };

#else
    commandLine += " > ";
    commandLine += quoteArgument(outputPath.string());
    commandLine += " 2>&1";

    const int rawResult = std::system(commandLine.c_str());

    int exitCode = rawResult;

    if (rawResult == -1) {
        exitCode = -1;
    } else if (WIFEXITED(rawResult)) {
        exitCode = WEXITSTATUS(rawResult);
    } else {
        exitCode = 128;
    }

    return {
        exitCode,
        readFile(outputPath)
    };
#endif
}

std::optional<std::string> extractValueAfter(
    const std::string& output,
    const std::string& prefix
) {
    const std::size_t start = output.find(prefix);

    if (start == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t valueStart = start + prefix.size();
    const std::size_t valueEnd =
        output.find_first_of("\r\n", valueStart);

    const std::string value = output.substr(
        valueStart,
        valueEnd == std::string::npos
            ? std::string::npos
            : valueEnd - valueStart
    );

    if (value.empty()) {
        return std::nullopt;
    }

    return value;
}

std::vector<std::string> submitArguments(
    const std::string& type,
    const std::string& payload,
    const std::filesystem::path& storagePath
) {
    return {
        "submit",
        "--type", type,
        "--payload", payload,
        "--priority", "normal",
        "--retries", "0",
        "--storage", storagePath.string()
    };
}

std::vector<std::string> statusArguments(
    const std::string& jobId,
    const std::filesystem::path& storagePath
) {
    return {
        "status",
        jobId,
        "--storage", storagePath.string()
    };
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "     JobQ CLI Workflow Tests\n";
    std::cout << "========================================\n\n";

    if (argc != 2) {
        std::cerr
            << "Usage: jobq_cli_workflow_test <path-to-jobq_cli>\n";
        return 2;
    }

    const std::filesystem::path executable = argv[1];

    if (!std::filesystem::exists(executable)) {
        std::cerr
            << "CLI executable does not exist: "
            << executable.string()
            << '\n';

        return 2;
    }

    const auto uniqueSuffix =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto testDirectory =
        std::filesystem::temp_directory_path() /
        ("jobq_cli_workflow_" + std::to_string(uniqueSuffix));

    const auto storagePath = testDirectory / "jobs.journal";

    std::error_code filesystemError;
    std::filesystem::create_directories(
        testDirectory,
        filesystemError
    );

    if (filesystemError ||
        !std::filesystem::exists(testDirectory)) {
        std::cerr
            << "Unable to create workflow test directory: "
            << testDirectory.string()
            << '\n';

        return 2;
    }

    // 1. Submit three jobs through separate CLI processes.

    const auto reportSubmit = runCli(
        executable,
        submitArguments("REPORT", "workflow-report", storagePath),
        testDirectory / "submit_report.txt"
    );

    const auto emailSubmit = runCli(
        executable,
        submitArguments("EMAIL", "workflow-email", storagePath),
        testDirectory / "submit_email.txt"
    );

    const auto failureSubmit = runCli(
        executable,
        submitArguments(
            "FAIL",
            "workflow-intentional-failure",
            storagePath
        ),
        testDirectory / "submit_failure.txt"
    );

    check(
        reportSubmit.exitCode == 0 &&
            reportSubmit.output.find("Job submitted successfully.") !=
                std::string::npos,
        "CLI submits a REPORT job"
    );

    check(
        emailSubmit.exitCode == 0 &&
            emailSubmit.output.find("Job submitted successfully.") !=
                std::string::npos,
        "CLI submits an EMAIL job"
    );

    check(
        failureSubmit.exitCode == 0 &&
            failureSubmit.output.find("Job submitted successfully.") !=
                std::string::npos,
        "CLI accepts a job configured to fail during execution"
    );

    // Print the subprocess diagnostics if any submission fails.
    if (reportSubmit.exitCode != 0) {
        std::cout
            << "\nREPORT submit diagnostic:\n"
            << reportSubmit.output << '\n';
    }

    if (emailSubmit.exitCode != 0) {
        std::cout
            << "\nEMAIL submit diagnostic:\n"
            << emailSubmit.output << '\n';
    }

    if (failureSubmit.exitCode != 0) {
        std::cout
            << "\nFAIL submit diagnostic:\n"
            << failureSubmit.output << '\n';
    }

    const auto reportId =
        extractValueAfter(reportSubmit.output, "ID: ");

    const auto emailId =
        extractValueAfter(emailSubmit.output, "ID: ");

    const auto failureId =
        extractValueAfter(failureSubmit.output, "ID: ");

    check(
        reportId.has_value(),
        "REPORT submission returns a job ID"
    );

    check(
        emailId.has_value(),
        "EMAIL submission returns a job ID"
    );

    check(
        failureId.has_value(),
        "FAIL submission returns a job ID"
    );

    if (!reportId || !emailId || !failureId) {
        std::filesystem::remove_all(
            testDirectory,
            filesystemError
        );

        std::cerr
            << "\nWorkflow stopped because a submitted job ID "
            << "could not be read.\n";

        return 1;
    }

    check(
        *reportId != *emailId &&
            *reportId != *failureId &&
            *emailId != *failureId,
        "Each submitted job receives a distinct ID"
    );

    // 2. Run all persisted jobs through the CLI.

    const auto runResult = runCli(
        executable,
        {
            "run",
            "--workers", "3",
            "--storage", storagePath.string()
        },
        testDirectory / "run.txt"
    );

    check(
        runResult.exitCode == 0,
        "CLI run command exits successfully"
    );

    check(
        runResult.output.find("Starting JobQ with 3 workers") !=
            std::string::npos,
        "CLI run starts the requested number of workers"
    );

    check(
        runResult.output.find("JobQ stopped cleanly.") !=
            std::string::npos,
        "CLI run shuts down the workers cleanly"
    );

    check(
        runResult.output.find("Completed jobs: 2") !=
            std::string::npos,
        "CLI run reports two completed jobs"
    );

    check(
        runResult.output.find("Failed jobs: 1") !=
            std::string::npos,
        "CLI run reports one failed job"
    );

    check(
        runResult.output.find("Pending jobs: 0") !=
                std::string::npos &&
            runResult.output.find("Running jobs: 0") !=
                std::string::npos,
        "CLI run leaves no jobs pending or running"
    );

    check(
        runResult.output.find(
            "[CLI] Processing REPORT: workflow-report"
        ) != std::string::npos &&
        runResult.output.find(
            "[CLI] Sending EMAIL: workflow-email"
        ) != std::string::npos,
        "Registered REPORT and EMAIL handlers execute"
    );

    // 3. Inspect persisted statuses from separate CLI processes.

    const auto listResult = runCli(
        executable,
        {
            "list",
            "--storage", storagePath.string()
        },
        testDirectory / "list_all.txt"
    );

    check(
        listResult.exitCode == 0,
        "CLI lists persisted jobs successfully"
    );

    check(
        listResult.output.find("workflow-report") !=
                std::string::npos &&
            listResult.output.find("workflow-email") !=
                std::string::npos &&
            listResult.output.find(
                "workflow-intentional-failure"
            ) != std::string::npos,
        "CLI list shows all three submitted jobs"
    );

    check(
        listResult.output.find("Total displayed: 3") !=
            std::string::npos,
        "CLI list reports three persisted jobs"
    );

    const auto failedListResult = runCli(
        executable,
        {
            "list",
            "--status", "FAILED",
            "--storage", storagePath.string()
        },
        testDirectory / "list_failed.txt"
    );

    check(
        failedListResult.exitCode == 0,
        "CLI filters jobs by FAILED status"
    );

    check(
        failedListResult.output.find(
            "workflow-intentional-failure"
        ) != std::string::npos &&
        failedListResult.output.find("workflow-report") ==
            std::string::npos &&
        failedListResult.output.find("workflow-email") ==
            std::string::npos,
        "Failed-job filter returns only the failed job"
    );

    check(
        failedListResult.output.find("Total displayed: 1") !=
            std::string::npos,
        "Failed-job filter reports one matching job"
    );

    const auto reportStatus = runCli(
        executable,
        statusArguments(*reportId, storagePath),
        testDirectory / "status_report.txt"
    );

    const auto emailStatus = runCli(
        executable,
        statusArguments(*emailId, storagePath),
        testDirectory / "status_email.txt"
    );

    const auto failureStatus = runCli(
        executable,
        statusArguments(*failureId, storagePath),
        testDirectory / "status_failure.txt"
    );

    check(
        reportStatus.exitCode == 0 &&
            reportStatus.output.find("Status: COMPLETED") !=
                std::string::npos,
        "REPORT status is persisted as COMPLETED"
    );

    check(
        emailStatus.exitCode == 0 &&
            emailStatus.output.find("Status: COMPLETED") !=
                std::string::npos,
        "EMAIL status is persisted as COMPLETED"
    );

    check(
        failureStatus.exitCode == 0 &&
            failureStatus.output.find("Status: FAILED") !=
                std::string::npos,
        "FAIL status is persisted as FAILED"
    );


    const auto secondRun = runCli(
        executable,
        {
            "run",
            "--workers", "2",
            "--storage", storagePath.string()
        },
        testDirectory / "run_again.txt"
    );

    check(
        secondRun.exitCode == 0 &&
            secondRun.output.find("Completed jobs: 2") !=
                std::string::npos &&
            secondRun.output.find("Failed jobs: 1") !=
                std::string::npos,
        "A second run preserves terminal job counts"
    );

    const auto emailStatusAfterSecondRun = runCli(
        executable,
        statusArguments(*emailId, storagePath),
        testDirectory / "status_email_after_second_run.txt"
    );

    check(
        emailStatusAfterSecondRun.exitCode == 0 &&
            emailStatusAfterSecondRun.output.find(
                "Status: COMPLETED"
            ) != std::string::npos &&
            emailStatusAfterSecondRun.output.find(
                "Attempts made: 1"
            ) != std::string::npos,
        "Completed jobs are not executed a second time"
    );

    std::filesystem::remove_all(
        testDirectory,
        filesystemError
    );

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';

    std::cout
        << (testsFailed == 0
                ? "Result: ALL TESTS PASSED\n"
                : "Result: TEST FAILURES DETECTED\n");

    std::cout << "========================================\n";

    return testsFailed == 0 ? 0 : 1;
}