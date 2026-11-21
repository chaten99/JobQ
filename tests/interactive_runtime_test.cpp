#include <sstream>
#include <stdexcept>
#include <string>

#include "cli/InteractiveRuntime.h"
#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;

std::size_t occurrences(const std::string& text, const std::string& fragment) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(fragment, position)) != std::string::npos) {
        ++count;
        position += fragment.size();
    }
    return count;
}

void testPersistentSession() {
    jobq::cli::InteractiveRuntime runtime({.workerCount = 1});
    jobq::Engine* firstEngine = nullptr;
    unsigned commands = 0;
    runtime.commands().registerCommand("observe", "Observe engine identity.",
        [&](const auto&, auto&) {
            auto* engine = &runtime.engine();
            if (!firstEngine) {
                firstEngine = engine;
            }
            check(engine == firstEngine && engine->isRunning(),
                  "Each command sees the same running Engine");
            check(jobq_test::throwsWith<std::logic_error>([&] { engine->start(); }, "once"),
                  "The session Engine has already been started exactly once");
            ++commands;
            return jobq::cli::CommandAction::Continue;
        });
    std::istringstream input("\n  help \nobserve\ninvalid-command\nhelp extra\nexit extra\nhelp 'broken\nobserve\nhelp\nexit\nquit\n");
    std::ostringstream output;
    std::ostringstream errors;
    check(runtime.run(input, output, errors) == 0, "Recoverable command errors preserve the session");
    check(commands == 2 && firstEngine && !firstEngine->isRunning(),
          "Two commands share one Engine and exit joins its workers");
    check(occurrences(output.str(), "Runtime started.") == 1 &&
          occurrences(output.str(), "Shutting down JobQ...") == 1 &&
          occurrences(output.str(), "Runtime stopped.") == 1,
          "Startup and explicit shutdown are reported once");
    check(occurrences(output.str(), "Available commands:") == 2 &&
          errors.str().find("Unknown command") != std::string::npos &&
          errors.str().find("does not accept arguments") != std::string::npos &&
          errors.str().find("Unterminated") != std::string::npos,
          "Help, unknown names, invalid arguments, and malformed input are handled");
    check(runtime.run(input, output, errors) == 1 &&
          occurrences(output.str(), "Runtime started.") == 1,
          "A second session cannot restart a stopped Engine");
}

void testExitAndEof() {
    for (const auto* commands : {"", "\n\t \n", "help", "quit\nexit\n", "exit\nexit\n"}) {
        jobq::cli::InteractiveRuntime runtime({.workerCount = 1});
        std::istringstream input(commands);
        std::ostringstream output;
        std::ostringstream errors;
        check(runtime.run(input, output, errors) == 0 && errors.str().empty() &&
              !runtime.engine().isRunning(), "EOF, empty input, exit, and quit stop safely");
        check(occurrences(output.str(), "Runtime stopped.") == 1,
              "Trailing repeated exit requests do not duplicate shutdown");
    }
}

void testFailuresAndCleanup() {
    jobq::cli::InteractiveRuntime invalid({.workerCount = 0});
    std::istringstream input("help\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    check(invalid.run(input, output, errors) == 1 &&
          errors.str().find("Unable to start runtime") != std::string::npos &&
          output.str().find("Runtime started.") == std::string::npos &&
          output.str().find("jobq>") == std::string::npos,
          "Invalid startup configuration reports failure without entering a session");
    check(jobq_test::throwsWith<std::logic_error>([&] { invalid.engine(); }, "not been initialized"),
          "No partially initialized Engine is exposed after construction failure");

    jobq::cli::InteractiveRuntime runtime({.workerCount = 1});
    runtime.commands().registerCommand("failure", "Throw a command failure.",
        [](const auto&, auto&) -> jobq::cli::CommandAction {
            throw std::runtime_error("command failure");
        });
    runtime.commands().registerCommand("nonstandard", "Throw a non-standard failure.",
        [](const auto&, auto&) -> jobq::cli::CommandAction { throw 42; });
    std::istringstream commands("failure\nnonstandard\nhelp\nexit\n");
    output.str("");
    errors.str("");
    check(runtime.run(commands, output, errors) == 0 &&
          errors.str().find("command failure") != std::string::npos &&
          errors.str().find("unknown error") != std::string::npos &&
          output.str().find("Available commands:") != std::string::npos,
          "Command exceptions are reported and later commands still run");

    jobq::cli::InteractiveRuntime badInput({.workerCount = 1});
    std::istringstream broken;
    broken.setstate(std::ios::badbit);
    output.str("");
    errors.str("");
    check(badInput.run(broken, output, errors) == 1 && !badInput.engine().isRunning() &&
          errors.str().find("Unable to read") != std::string::npos,
          "Input failure reports an error and joins workers");

    jobq::cli::InteractiveRuntime badOutput({.workerCount = 1});
    badOutput.commands().registerCommand("break-output", "Break the injected stream.",
        [](const auto&, std::ostream& stream) {
            stream.exceptions(std::ios::badbit | std::ios::failbit);
            stream.setstate(std::ios::badbit);
            return jobq::cli::CommandAction::Continue;
        });
    std::istringstream outputCommands("break-output\n");
    std::ostringstream failedOutput;
    errors.str("");
    check(badOutput.run(outputCommands, failedOutput, errors) == 1 &&
          !badOutput.engine().isRunning() && !errors.str().empty(),
          "Throwing output failures still stop the Engine before run returns");
}

}

int main() {
    try {
        testPersistentSession();
        testExitAndEof();
        testFailuresAndCleanup();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
