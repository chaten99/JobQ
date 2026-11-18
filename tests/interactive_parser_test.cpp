#include <string>
#include <vector>

#include "cli/InteractiveCommand.h"
#include "helpers/TestSupport.h"

int main() {
    using jobq_test::check;
    jobq::cli::InteractiveCommand command;
    std::string error;
    check(jobq::cli::parseInteractiveCommand("  help \t ", command, error) &&
          command.name == "help" && command.arguments.empty(),
          "Whitespace is trimmed around command names");
    check(jobq::cli::parseInteractiveCommand("inspect  42\t--format json", command, error) &&
          command.name == "inspect" &&
          command.arguments == std::vector<std::string>{"42", "--format", "json"},
          "Positional and option tokens are forwarded in order");
    check(jobq::cli::parseInteractiveCommand("inspect 'two words' \"\" C:\\jobs\\data", command, error) &&
          command.arguments == std::vector<std::string>{"two words", "", "C:\\jobs\\data"},
          "Quoted arguments preserve whitespace and empty values, backslashes are literal");
    check(jobq::cli::parseInteractiveCommand("inspect pre\"two words\"post", command, error) &&
          command.arguments == std::vector<std::string>{"pretwo wordspost"},
          "Adjacent quoted and unquoted fragments form one argument");
    check(jobq::cli::parseInteractiveCommand("inspect \"it's ready\"", command, error) &&
          command.arguments == std::vector<std::string>{"it's ready"},
          "The other quote character is literal inside quoted arguments");
    for (const auto* input : {"", " \t\r ", "\n"}) {
        check(jobq::cli::parseInteractiveCommand(input, command, error) &&
              command.name.empty() && command.arguments.empty() && error.empty(),
              "Empty input produces an empty command");
    }
    check(jobq::cli::parseInteractiveCommand("unknown", command, error) &&
          command.name == "unknown", "Parser preserves unknown names for registry lookup");
    for (const auto* input : {"help 'broken", "help \"broken", "\"\" value"}) {
        check(!jobq::cli::parseInteractiveCommand(input, command, error) &&
              command.name.empty() && !error.empty(),
              "Malformed quotes or an empty command name are rejected without partial output");
    }
    check(!jobq::cli::parseInteractiveCommand(std::string("help\0exit", 9), command, error),
          "Embedded NUL is rejected");
    check(!jobq::cli::parseInteractiveCommand("help\x1b", command, error),
          "Terminal control characters are rejected");
    check(jobq::cli::parseInteractiveCommand("quit", command, error) && error.empty(),
          "A valid command clears earlier parser errors");
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
