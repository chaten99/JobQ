#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <csignal>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/InteractiveRuntime.h"
#include "helpers/TestSupport.h"

namespace {

struct Handle {
    HANDLE value{nullptr};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

BOOL WINAPI ignoreControl(DWORD) { return TRUE; }

int child(const std::string& mode, HANDLE ready, HANDLE release) {
    Handle consoleInput;
    if (mode == "console" || mode == "console-eof") {
        consoleInput.value = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (consoleInput.value == INVALID_HANDLE_VALUE || !SetStdHandle(STD_INPUT_HANDLE, consoleInput.value)) return 1;
    }
    jobq_test::Counter entered;
    jobq::cli::InteractiveRuntime runtime({.workerCount = 2}, [&](jobq::Engine& engine) {
        engine.registerHandler("BLOCK", [&](const jobq::Job&) {
            entered.add();
            if (WaitForSingleObject(release, 10000) != WAIT_OBJECT_0) throw std::runtime_error("Release timed out");
        });
        engine.registerHandler("OK", [](const jobq::Job&) {});
    });
    runtime.commands().registerCommand("ready", "Prepare signal fixture.", [&](const auto&, auto& output) {
        if (mode == "active" || mode == "race") {
            runtime.submitJob("BLOCK", "first");
            runtime.submitJob("BLOCK", "second");
            if (!entered.waitAtLeast(2)) throw std::runtime_error("Workers not ready");
            runtime.submitJob("OK", "queued first");
            runtime.submitJob("OK", "queued second");
        }
        SetEvent(ready);
        if (mode == "term") std::raise(SIGTERM);
        if (mode == "monitor") return runtime.commands().dispatch({"monitor", {"10000", "6"}}, output);
        return jobq::cli::CommandAction::Continue;
    });
    std::cout << "Fixture console ready.\n" << std::flush;
    const auto result = runtime.runTerminal(std::cout, std::cerr);
    if (mode == "active" || mode == "race") {
        const auto metrics = runtime.engine().metrics();
        if (metrics.submittedJobs != 4 || metrics.completedExecutions != 4 || metrics.startedExecutions != 4) return 1;
        for (const auto& job : runtime.engine().listJobs()) {
            if (job.status != jobq::JobStatus::Completed) return 1;
        }
        std::cout << "All four accepted jobs drained.\n";
    }
    return result;
}

void ensure(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message + " (Windows error " + std::to_string(GetLastError()) + ")");
}

void scenario(const std::wstring& executable, const std::string& mode, DWORD signal, int expected) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle inputRead, inputWrite, outputRead, outputWrite;
    ensure(CreatePipe(&inputRead.value, &inputWrite.value, &security, 0), "Create input pipe");
    ensure(CreatePipe(&outputRead.value, &outputWrite.value, &security, 0), "Create output pipe");
    SetHandleInformation(inputWrite.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outputRead.value, HANDLE_FLAG_INHERIT, 0);
    Handle ready{CreateEventW(&security, TRUE, FALSE, nullptr)};
    Handle release{CreateEventW(&security, TRUE, FALSE, nullptr)};
    ensure(ready.value && release.value, "Create readiness events");
    std::wostringstream command;
    command << L'"' << executable << L"\" --child " << std::wstring(mode.begin(), mode.end()) << L' '
            << reinterpret_cast<std::uintptr_t>(ready.value) << L' '
            << reinterpret_cast<std::uintptr_t>(release.value);
    auto text = command.str();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = inputRead.value;
    startup.hStdOutput = outputWrite.value;
    startup.hStdError = outputWrite.value;
    PROCESS_INFORMATION process{};
    ensure(CreateProcessW(nullptr, text.data(), nullptr, nullptr, TRUE, CREATE_NEW_CONSOLE,
                          nullptr, nullptr, &startup, &process), "Create child console");
    Handle processHandle{process.hProcess}, threadHandle{process.hThread};
    CloseHandle(outputWrite.value);
    outputWrite.value = nullptr;
    std::string output;
    auto readAvailable = [&] {
        DWORD available = 0;
        if (!PeekNamedPipe(outputRead.value, nullptr, 0, nullptr, &available, nullptr)) return;
        while (available) {
            char bytes[4096];
            DWORD count = 0;
            if (!ReadFile(outputRead.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr)) break;
            output.append(bytes, count);
            if (!PeekNamedPipe(outputRead.value, nullptr, 0, nullptr, &available, nullptr)) break;
        }
    };
    auto untilText = [&](const std::string& marker) {
        const auto deadline = GetTickCount64() + 5000;
        do {
            readAvailable();
            if (output.find(marker) != std::string::npos) return true;
            if (WaitForSingleObject(processHandle.value, 5) == WAIT_OBJECT_0) break;
        } while (GetTickCount64() < deadline);
        readAvailable();
        return output.find(marker) != std::string::npos;
    };
    try {
        DWORD written = 0;
        ensure(untilText("Fixture console ready."), "Wait for console creation");
        FreeConsole();
        ensure(AttachConsole(process.dwProcessId), "Attach child console");
        ensure(SetConsoleCtrlHandler(ignoreControl, TRUE), "Protect controller from control events");
        auto consoleWrite = [&](const std::wstring& text) {
            Handle console{CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
            ensure(console.value != INVALID_HANDLE_VALUE, "Open console input");
            std::vector<INPUT_RECORD> keys;
            for (const auto character : text) {
                INPUT_RECORD key{};
                key.EventType = KEY_EVENT;
                key.Event.KeyEvent.bKeyDown = TRUE;
                key.Event.KeyEvent.wRepeatCount = 1;
                key.Event.KeyEvent.uChar.UnicodeChar = character;
                key.Event.KeyEvent.wVirtualKeyCode = character == L'\r' ? VK_RETURN : 0;
                keys.push_back(key);
            }
            DWORD count = 0;
            ensure(WriteConsoleInputW(console.value, keys.data(), static_cast<DWORD>(keys.size()), &count),
                   "Write real console input records");
        };
        if (mode == "console" || mode == "console-eof") consoleWrite(L"ready\r");
        else ensure(WriteFile(inputWrite.value, "ready\n", 6, &written, nullptr), "Send ready command");
        ensure(WaitForSingleObject(ready.value, 5000) == WAIT_OBJECT_0, "Wait for child readiness: " + output);
        if (mode == "monitor") ensure(untilText("Baseline"), "Wait for monitoring baseline: " + output);
        if (mode == "console") consoleWrite(L"unfinished");
        if (mode == "console-eof") consoleWrite(L"\x1a\r");
        else if (mode != "term") ensure(GenerateConsoleCtrlEvent(signal, 0), "Deliver control event");
        if (mode == "race") WriteFile(inputWrite.value, "exit\n", 5, &written, nullptr);
        ensure(untilText("Shutting down JobQ..."), "Wait for coordinated shutdown: " + output);
        if (mode == "active" || mode == "race") {
            ensure(WaitForSingleObject(processHandle.value, 0) == WAIT_TIMEOUT, "Child must wait for active jobs");
            ensure(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "Deliver repeated control request");
            SetEvent(release.value);
        }
        ensure(WaitForSingleObject(processHandle.value, 5000) == WAIT_OBJECT_0, "Wait for graceful child exit");
        readAvailable();
        DWORD code = 0;
        GetExitCodeProcess(processHandle.value, &code);
        ensure(code == static_cast<DWORD>(expected), "Unexpected child exit " + std::to_string(code) + ": " + output);
        ensure(output.find("Runtime stopped.") != std::string::npos, "Missing successful shutdown output");
        if (mode == "monitor") {
            ensure(output.find("Monitoring interrupted") != std::string::npos &&
                   output.find("Monitoring completed.") == std::string::npos, "Monitor must report interruption");
        }
        if (mode == "active" || mode == "race") {
            ensure(output.find("All four accepted jobs drained.") != std::string::npos, "Accepted jobs did not drain");
        }
        SetConsoleCtrlHandler(ignoreControl, FALSE);
        FreeConsole();
    } catch (...) {
        SetEvent(release.value);
        if (WaitForSingleObject(processHandle.value, 1000) != WAIT_OBJECT_0) {
            TerminateProcess(processHandle.value, 1);
            WaitForSingleObject(processHandle.value, 5000);
        }
        SetConsoleCtrlHandler(ignoreControl, FALSE);
        FreeConsole();
        throw;
    }
    std::cout << "[PASS] Console signal scenario " << mode << '\n';
}

}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--child") {
        return child(argv[2], reinterpret_cast<HANDLE>(std::stoull(argv[3])),
                     reinterpret_cast<HANDLE>(std::stoull(argv[4])));
    }
    try {
        wchar_t executable[32768];
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        ensure(length > 0 && length < 32768, "Get fixture executable path");
        const std::wstring path(executable, length);
        scenario(path, "idle", CTRL_C_EVENT, 130);
        scenario(path, "console", CTRL_C_EVENT, 130);
        scenario(path, "console-eof", CTRL_C_EVENT, 0);
        scenario(path, "break", CTRL_BREAK_EVENT, 128 + SIGBREAK);
        scenario(path, "term", CTRL_C_EVENT, 143);
        scenario(path, "monitor", CTRL_C_EVENT, 130);
        scenario(path, "active", CTRL_C_EVENT, 130);
        scenario(path, "race", CTRL_C_EVENT, 130);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
