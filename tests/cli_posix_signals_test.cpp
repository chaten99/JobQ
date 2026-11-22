#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

#include "cli/InteractiveRuntime.h"
#include "helpers/TestSupport.h"

namespace {

void ensure(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int fixture(const std::string& mode, int release) {
    jobq_test::Counter entered;
    jobq::cli::InteractiveRuntime runtime({.workerCount = 2}, [&](jobq::Engine& engine) {
        engine.registerHandler("BLOCK", [&](const jobq::Job&) {
            entered.add();
            struct pollfd input{release, POLLIN, 0};
            char byte;
            int available;
            do { available = poll(&input, 1, 10000); } while (available < 0 && errno == EINTR);
            ssize_t count = -1;
            if (available == 1) {
                do { count = read(release, &byte, 1); } while (count < 0 && errno == EINTR);
            }
            if (count != 1) {
                throw std::runtime_error("Handler release failed");
            }
        });
        engine.registerHandler("OK", [](const jobq::Job&) {});
    });
    runtime.commands().registerCommand("ready", "Prepare signal fixture.", [&](const auto&, auto& output) {
        if (mode == "active" || mode == "repeat" || mode == "race") {
            runtime.submitJob("BLOCK", "first");
            runtime.submitJob("BLOCK", "second");
            if (!entered.waitAtLeast(2)) throw std::runtime_error("Workers not ready");
            runtime.submitJob("OK", "queued first");
            runtime.submitJob("OK", "queued second");
        }
        output << "Prepared.\n" << std::flush;
        if (mode == "monitor") return runtime.commands().dispatch({"monitor", {"10000", "6"}}, output);
        return jobq::cli::CommandAction::Continue;
    });
    const auto result = runtime.runTerminal(std::cout, std::cerr);
    if (mode == "active" || mode == "repeat" || mode == "race") {
        const auto metrics = runtime.engine().metrics();
        if (metrics.submittedJobs != 4 || metrics.completedExecutions != 4) return 1;
    }
    return result;
}

void scenario(const std::string& mode, int signal) {
    int input[2], output[2], release[2];
    ensure(pipe(input) == 0 && pipe(output) == 0 && pipe(release) == 0, "Create subprocess pipes");
    std::cout.flush();
    const auto child = fork();
    ensure(child >= 0, "Fork signal fixture");
    if (child == 0) {
        close(input[1]); close(output[0]); close(release[1]);
        dup2(input[0], STDIN_FILENO);
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(input[0]); close(output[1]);
        int result = 1;
        try { result = fixture(mode, release[0]); } catch (...) { }
        close(release[0]);
        std::cout.flush();
        std::cerr.flush();
        _exit(result);
    }
    close(input[0]); close(output[1]); close(release[0]);
    bool joined = false;
    std::string text;
    auto until = [&](const std::string& marker) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (text.find(marker) != std::string::npos) return true;
            struct pollfd stream{output[0], POLLIN, 0};
            const auto ready = poll(&stream, 1, 100);
            if (ready < 0) continue;
            if (ready) {
                char bytes[4096];
                const auto count = read(output[0], bytes, sizeof(bytes));
                if (count <= 0) break;
                text.append(bytes, static_cast<std::size_t>(count));
            }
        }
        return text.find(marker) != std::string::npos;
    };
    try {
        ensure(write(input[1], "ready\n", 6) == 6, "Write fixture command");
        ensure(until(mode == "monitor" ? "Baseline" : "Prepared."), "Wait for readiness: " + text);
        ensure(kill(child, signal) == 0, "Deliver process signal");
        if (mode == "race") ensure(write(input[1], "exit\n", 5) == 5, "Race exit and signal");
        ensure(until("Shutting down JobQ..."), "Wait for shutdown request: " + text);
        if (mode == "active" || mode == "repeat" || mode == "race") {
            int status = 0;
            ensure(waitpid(child, &status, WNOHANG) == 0, "Active workers must remain alive until released");
            if (mode == "repeat") ensure(kill(child, signal) == 0, "Repeat signal request");
            ensure(write(release[1], "xx", 2) == 2, "Release active handlers");
        }
        ensure(until("Runtime stopped."), "Wait for joined shutdown: " + text);
        int status = 0;
        const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < joinDeadline) {
            if (waitpid(child, &status, WNOHANG) == child) { joined = true; break; }
            poll(nullptr, 0, 5);
        }
        ensure(joined, "Join signal fixture");
        ensure(WIFEXITED(status) && WEXITSTATUS(status) == 128 + signal, "Incorrect signal exit status");
        if (mode == "monitor") ensure(text.find("Monitoring completed.") == std::string::npos,
                                      "Cancelled monitor must not report completion");
    } catch (...) {
        if (!joined) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        close(input[1]); close(output[0]); close(release[1]);
        throw;
    }
    close(input[1]); close(output[0]); close(release[1]);
    std::cout << "[PASS] POSIX signal scenario " << mode << '\n';
}

}

int main() {
    try {
        scenario("idle", SIGINT);
        scenario("idle", SIGTERM);
        scenario("active", SIGINT);
        scenario("monitor", SIGTERM);
        scenario("repeat", SIGINT);
        scenario("race", SIGINT);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
