#include "cli/InteractiveRuntime.h"

#include <exception>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "cli/CommandLine.h"
#include "cli/InspectionCommands.h"
#include "cli/JobCommands.h"

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

int InteractiveRuntime::run(std::istream& input, std::ostream& output, std::ostream& errorOutput) {
    {
        std::unique_lock lock(mutex_);
        if (used_) {
            lock.unlock();
            errorOutput << "Error: An interactive runtime can only run once.\n";
            return kCommandErrorExitCode;
        }
        used_ = true;
        state_ = RuntimeState::Starting;
    }

    bool started = false;
    int result = kSuccessExitCode;
    try {
        output << "JobQ Interactive Runtime\n";
        auto engine = std::make_unique<Engine>(config_);
        if (initializeEngine_) {
            initializeEngine_(*engine);
        }
        {
            std::lock_guard lock(mutex_);
            engine_ = std::move(engine);
        }
        output << "Engine initialized successfully.\n";
        engine_->start();
        started = true;
        setState(RuntimeState::Running);
        output << "Runtime started.\n";
        std::string line;
        while (true) {
            output << "jobq> " << std::flush;
            if (!output) {
                throw std::runtime_error("Unable to write the runtime prompt");
            }
            if (!std::getline(input, line)) {
                if (input.bad() || !input.eof()) {
                    throw std::runtime_error("Unable to read terminal input");
                }
                output << '\n';
                break;
            }

            InteractiveCommand command;
            std::string error;
            if (!parseInteractiveCommand(line, command, error)) {
                errorOutput << "Error: " << error << '\n';
                continue;
            }
            try {
                if (commands_.dispatch(command, output) == CommandAction::Exit) {
                    break;
                }
            } catch (const std::exception& failure) {
                errorOutput << "Error: " << failure.what() << '\n';
            } catch (...) {
                errorOutput << "Error: Command failed with an unknown error.\n";
            }
        }
    } catch (const std::exception& error) {
        errorOutput << (started ? "Runtime input/output failure: " : "Unable to start runtime: ")
                    << error.what() << '\n';
        result = kCommandErrorExitCode;
    } catch (...) {
        errorOutput << (started ? "Runtime input/output failure: " : "Unable to start runtime: ")
                    << "unknown error.\n";
        result = kCommandErrorExitCode;
    }

    if (!started) {
        setState(RuntimeState::Failed);
        return result;
    }

    setState(RuntimeState::Stopping);
    try {
        output << "Shutting down JobQ...\n";
    } catch (const std::exception& error) {
        errorOutput << "Unable to write shutdown output: " << error.what() << '\n';
        result = kCommandErrorExitCode;
    }
    bool stopped = false;
    try {
        engine_->stop();
        stopped = true;
        setState(RuntimeState::Stopped);
    } catch (const std::exception& error) {
        setState(RuntimeState::Failed);
        errorOutput << "Unable to stop runtime: " << error.what() << '\n';
        result = kCommandErrorExitCode;
    } catch (...) {
        setState(RuntimeState::Failed);
        errorOutput << "Unable to stop runtime: unknown error.\n";
        result = kCommandErrorExitCode;
    }
    if (stopped) {
        try {
            output << "Runtime stopped.\n";
            if (!output) {
                throw std::runtime_error("Unable to write shutdown output");
            }
        } catch (const std::exception& error) {
            errorOutput << "Unable to write shutdown output: " << error.what() << '\n';
            result = kCommandErrorExitCode;
        }
    }
    return result;
}

}
