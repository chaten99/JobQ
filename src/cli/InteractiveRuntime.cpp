#include "cli/InteractiveRuntime.h"

#include <exception>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "cli/CommandLine.h"

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

InteractiveRuntime::InteractiveRuntime(EngineConfig config)
    : config_(std::move(config)) {
    commands_.registerCommand("help", "Display available commands.",
        [this](const InteractiveCommand& command, std::ostream& output) {
            requireNoArguments(command);
            commands_.printHelp(output);
            return CommandAction::Continue;
        });
    commands_.registerCommand("exit", "Stop the runtime and exit.", requestExit);
    commands_.registerCommand("quit", "Alias for exit.", requestExit);
}

CommandDispatcher& InteractiveRuntime::commands() noexcept {
    return commands_;
}

Engine& InteractiveRuntime::engine() {
    if (!engine_) {
        throw std::logic_error("Runtime engine has not been initialized");
    }
    return *engine_;
}

int InteractiveRuntime::run(std::istream& input, std::ostream& output, std::ostream& errorOutput) {
    if (used_) {
        errorOutput << "Error: An interactive runtime can only run once.\n";
        return kCommandErrorExitCode;
    }
    used_ = true;

    bool started = false;
    int result = kSuccessExitCode;
    try {
        output << "JobQ Interactive Runtime\n";
        engine_ = std::make_unique<Engine>(config_);
        output << "Engine initialized successfully.\n";
        engine_->start();
        started = true;
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
        return result;
    }

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
    } catch (const std::exception& error) {
        errorOutput << "Unable to stop runtime: " << error.what() << '\n';
        result = kCommandErrorExitCode;
    } catch (...) {
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
