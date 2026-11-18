#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/CommandDispatcher.h"
#include "helpers/TestSupport.h"

int main() {
    using jobq::cli::CommandAction;
    using jobq_test::check;
    using jobq_test::throwsWith;
    jobq::cli::CommandDispatcher dispatcher;
    std::ostringstream output;
    unsigned calls = 0;
    dispatcher.registerCommand("inspect", "Inspect a test value.",
        [&](const jobq::cli::InteractiveCommand& command, std::ostream& stream) {
            ++calls;
            check(command.arguments == std::vector<std::string>{"42", "--format", "text"},
                  "Dispatch forwards all arguments unchanged");
            stream << "inspected";
            return CommandAction::Continue;
        });
    dispatcher.registerCommand("exit", "Exit the test.",
        [](const jobq::cli::InteractiveCommand&, std::ostream&) { return CommandAction::Exit; });
    check(dispatcher.dispatch({"inspect", {"42", "--format", "text"}}, output) ==
          CommandAction::Continue && calls == 1 && output.str() == "inspected",
          "Registered command executes once and writes to the supplied stream");
    check(dispatcher.dispatch({}, output) == CommandAction::Continue && calls == 1,
          "Empty commands are ignored");
    check(dispatcher.dispatch({"exit", {}}, output) == CommandAction::Exit,
          "Exit action reaches the caller");
    check(throwsWith<std::invalid_argument>([&] {
        dispatcher.dispatch({"missing", {}}, output);
    }, "Unknown command"), "Unknown commands produce actionable errors");
    check(throwsWith<std::invalid_argument>([&] {
        dispatcher.registerCommand("inspect", "Duplicate", [](const auto&, auto&) {
            return CommandAction::Exit;
        });
    }, "already registered"), "Duplicate registration is rejected");
    check(throwsWith<std::invalid_argument>([&] {
        dispatcher.registerCommand("invalid name", "Invalid", {});
    }, "non-empty"), "Missing command handlers are rejected");
    check(throwsWith<std::invalid_argument>([&] {
        dispatcher.registerCommand("invalid name", "Invalid", [](const auto&, auto&) {
            return CommandAction::Continue;
        });
    }, "Command names"), "Invalid command names are rejected");
    dispatcher.registerCommand("fail", "Fail deliberately.", [](const auto&, auto&) -> CommandAction {
        throw std::runtime_error("dispatch failure");
    });
    check(throwsWith<std::runtime_error>([&] {
        dispatcher.dispatch({"fail", {}}, output);
    }, "dispatch failure"), "Handler exceptions propagate to runtime reporting");
    output.str("");
    dispatcher.printHelp(output);
    check(output.str().find("inspect  Inspect a test value.") != std::string::npos &&
          output.str().find("exit  Exit the test.") != std::string::npos &&
          output.str().find("submit") == std::string::npos,
          "Registry help advertises only registered commands");
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
