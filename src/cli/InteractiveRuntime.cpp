#include "cli/InteractiveRuntime.h"

#include <exception>
#include <algorithm>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "cli/CommandLine.h"
#include "cli/InspectionCommands.h"
#include "cli/JobCommands.h"
#include "cli/MetricsCommands.h"
#include "cli/TerminalSession.h"

namespace jobq::cli {

namespace {

void requireNoArguments(const InteractiveCommand& command) {
    if (!command.arguments.empty()) {
        throw std::invalid_argument("Command '" + command.name + "' does not accept arguments.");
    }
}

CommandAction requestExit(const InteractiveCommand& command, std::ostream&) {
    requireNoArguments(command);
    return CommandAction::Exit;
}

}

InteractiveRuntime::InteractiveRuntime(EngineConfig config, EngineInitializer initializeEngine)
    : config_(std::move(config)), initializeEngine_(std::move(initializeEngine)) {
    commands_.registerCommand("help", "Display available commands.",
        [this](const InteractiveCommand& command, std::ostream& output) {
            if (command.arguments.size() > 1 ||
                (command.arguments.size() == 1 && command.arguments.front().empty())) {
                throw std::invalid_argument("Usage: help [command]");
            }
            commands_.printHelp(output, command.arguments.empty() ? "" : command.arguments.front());
            return CommandAction::Continue;
        }, "help [command]");
    commands_.registerCommand("exit", "Stop the runtime and exit.", requestExit);
    commands_.registerCommand("quit", "Alias for exit.", requestExit);
    registerInspectionCommands(*this);
    registerJobCommands(*this);
    registerMetricsCommands(*this);
}

CommandDispatcher& InteractiveRuntime::commands() noexcept {
    return commands_;
}

Engine& InteractiveRuntime::engine() {
    std::lock_guard lock(mutex_);
    if (!engine_) {
        throw std::logic_error("Runtime engine has not been initialized");
    }
    return *engine_;
}

void InteractiveRuntime::setState(RuntimeState state) {
    std::lock_guard lock(mutex_);
    if (state == RuntimeState::Running && state_ == RuntimeState::ShutdownRequested) return;
    state_ = state;
    if (state == RuntimeState::Running) {
        startedAt_ = std::chrono::steady_clock::now();
    } else if ((state == RuntimeState::Stopped || state == RuntimeState::Failed) && startedAt_) {
        stoppedAt_ = std::chrono::steady_clock::now();
    }
}

RuntimeSnapshot InteractiveRuntime::snapshot() const {
    RuntimeSnapshot result;
    Engine* engine = nullptr;
    {
        std::lock_guard lock(mutex_);
        result.state = state_;
        engine = engine_.get();
        if (startedAt_) {
            result.uptime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                stoppedAt_.value_or(std::chrono::steady_clock::now()) - *startedAt_
            );
        }
    }
    if (engine) {
        result.engine = engine->snapshot();
    }
    return result;
}

void InteractiveRuntime::requestShutdown(int signalNumber) {
    if (signalNumber < 0 || signalNumber > 127) throw std::invalid_argument("Invalid shutdown signal number");
    {
        std::lock_guard lock(mutex_);
        if (state_ == RuntimeState::Stopped || state_ == RuntimeState::Failed) return;
        if (signalNumber && !shutdownSignal_) shutdownSignal_ = signalNumber;
        if (state_ != RuntimeState::Stopping) state_ = RuntimeState::ShutdownRequested;
    }
    shutdownChanged_.notify_all();
}

bool InteractiveRuntime::shutdownRequested() {
    int signal = 0;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) signal = terminal_->signalNumber();
    }
    if (signal) requestShutdown(signal);
    std::lock_guard lock(mutex_);
    return state_ == RuntimeState::ShutdownRequested || state_ == RuntimeState::Stopping ||
           state_ == RuntimeState::Stopped || state_ == RuntimeState::Failed;
}

bool InteractiveRuntime::waitForShutdown(std::chrono::milliseconds interval) {
    const auto deadline = std::chrono::steady_clock::now() + interval;
    while (!shutdownRequested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        std::unique_lock lock(mutex_);
        shutdownChanged_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds{25}), [&] {
            return state_ == RuntimeState::ShutdownRequested || state_ == RuntimeState::Stopping ||
                   state_ == RuntimeState::Stopped || state_ == RuntimeState::Failed;
        });
    }
    return true;
}

JobId InteractiveRuntime::submitJob(std::string type, std::string payload) {
    if (shutdownRequested()) throw std::logic_error("Runtime is shutting down; submissions require a running runtime");
    std::lock_guard lock(mutex_);
    if (!engine_) throw std::logic_error("Runtime engine has not been initialized");
    if (state_ != RuntimeState::Running) throw std::logic_error("Jobs require a running runtime");
    return engine_->submit(std::move(type), std::move(payload));
}

int InteractiveRuntime::run(std::istream& input, std::ostream& output, std::ostream& errorOutput) {
    if (!beginRun(errorOutput)) return kCommandErrorExitCode;
    return runSession([&](std::string& line) {
        if (std::getline(input, line)) return true;
        if (!shutdownRequested() && (input.bad() || !input.eof())) throw std::runtime_error("Unable to read terminal input");
        return false;
    }, output, errorOutput);
}

int InteractiveRuntime::runTerminal(std::ostream& output, std::ostream& errorOutput) {
    if (!beginRun(errorOutput)) return kCommandErrorExitCode;
    int result = 1;
    std::unique_ptr<TerminalSession> terminal;
    try {
        terminal = std::make_unique<TerminalSession>();
        {
            std::lock_guard lock(mutex_);
            terminal_ = terminal.get();
        }
        result = runSession([&](std::string& line) {
            return terminal->readLine(line, [&] { return shutdownRequested(); },
                [&] { waitForShutdown(std::chrono::milliseconds{25}); });
        }, output, errorOutput);
        {
            std::lock_guard lock(mutex_);
            terminal_ = nullptr;
        }
        terminal->restore();
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            terminal_ = nullptr;
        }
        try { errorOutput << "Terminal lifecycle failure: " << error.what() << '\n'; } catch (...) { }
        if (terminal) {
            try { terminal->restore(); }
            catch (const std::exception& cleanup) {
                try { errorOutput << "Signal cleanup failure: " << cleanup.what() << '\n'; } catch (...) { }
            }
        }
        setState(RuntimeState::Failed);
        result = 1;
    }
    return result;
}

bool InteractiveRuntime::beginRun(std::ostream& errorOutput) {
    {
        std::unique_lock lock(mutex_);
        if (used_) {
            lock.unlock();
            try { errorOutput << "Error: An interactive runtime can only run once.\n"; } catch (...) { }
            return false;
        }
        used_ = true;
        if (state_ != RuntimeState::ShutdownRequested) state_ = RuntimeState::Starting;
    }
    return true;
}

int InteractiveRuntime::runSession(const std::function<bool(std::string&)>& readLine,
                                 std::ostream& output, std::ostream& errorOutput) {

    bool started = false;
    int result = kSuccessExitCode;
    auto failure = [&](const std::string& message) noexcept {
        result = kCommandErrorExitCode;
        try { errorOutput << message << '\n'; } catch (...) { }
    };
    try {
        output << "JobQ Interactive Runtime\n";
        auto engine = std::make_unique<Engine>(config_);
        {
            std::lock_guard lock(mutex_);
            engine_ = std::move(engine);
        }
        output << "Engine initialized successfully.\n";
        if (initializeEngine_) initializeEngine_(*engine_);
        if (!shutdownRequested()) {
            engine_->start();
            started = true;
            setState(RuntimeState::Running);
            output << "Runtime started.\n";
        }
        std::string line;
        while (!shutdownRequested()) {
            output << "jobq> " << std::flush;
            if (!output) {
                throw std::runtime_error("Unable to write the runtime prompt");
            }
            if (!readLine(line)) {
                output << '\n';
                requestShutdown();
                break;
            }
            if (shutdownRequested()) break;

            InteractiveCommand command;
            std::string error;
            if (!parseInteractiveCommand(line, command, error)) {
                errorOutput << "Error: " << error << '\n';
                continue;
            }
            if (shutdownRequested()) break;
            try {
                if (commands_.dispatch(command, output) == CommandAction::Exit) {
                    requestShutdown();
                    break;
                }
            } catch (const std::exception& failure) {
                errorOutput << "Error: " << failure.what() << '\n';
            } catch (...) {
                errorOutput << "Error: Command failed with an unknown error.\n";
            }
        }
    } catch (const std::exception& error) {
        failure(std::string(started ? "Runtime input/output failure: " : "Unable to start runtime: ") + error.what());
    } catch (...) {
        failure(started ? "Runtime input/output failure: unknown error." : "Unable to start runtime: unknown error.");
    }

    requestShutdown();
    if (!engine_) {
        setState(RuntimeState::Failed);
        return result;
    }

    setState(RuntimeState::Stopping);
    try {
        output << "Shutting down JobQ...\n" << std::flush;
        if (!output) throw std::runtime_error("Unable to write shutdown output");
    } catch (const std::exception& error) {
        failure(std::string("Unable to write shutdown output: ") + error.what());
    } catch (...) {
        failure("Unable to write shutdown output: unknown error.");
    }
    bool stopped = false;
    try {
        engine_->stop();
        stopped = true;
    } catch (const std::exception& error) {
        failure(std::string("Unable to stop runtime: ") + error.what());
    } catch (...) {
        failure("Unable to stop runtime: unknown error.");
    }
    if (stopped) {
        try {
            output << "Runtime stopped.\n";
            if (!output) {
                throw std::runtime_error("Unable to write shutdown output");
            }
        } catch (const std::exception& error) {
            failure(std::string("Unable to write shutdown output: ") + error.what());
        } catch (...) {
            failure("Unable to write shutdown output: unknown error.");
        }
    }
    shutdownRequested();
    setState(result ? RuntimeState::Failed : RuntimeState::Stopped);
    if (!result) {
        std::lock_guard lock(mutex_);
        if (shutdownSignal_) return 128 + shutdownSignal_;
    }
    return result;
}

}
