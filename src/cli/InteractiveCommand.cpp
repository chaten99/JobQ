#include "cli/InteractiveCommand.h"

#include <cctype>
#include <utility>

namespace jobq::cli {

bool parseInteractiveCommand(
    std::string_view input,
    InteractiveCommand& command,
    std::string& error
) {
    command = {};
    error.clear();
    std::vector<std::string> tokens;
    std::string token;
    char quote = 0;
    bool tokenStarted = false;

    for (const unsigned char character : input) {
        if (std::iscntrl(character) && !std::isspace(character)) {
            error = "Control characters are not allowed in commands.";
            return false;
        }
        if (quote != 0) {
            if (character == static_cast<unsigned char>(quote)) {
                quote = 0;
            } else {
                token.push_back(static_cast<char>(character));
            }
        } else if (character == '\'' || character == '"') {
            quote = static_cast<char>(character);
            tokenStarted = true;
        } else if (std::isspace(character)) {
            if (tokenStarted) {
                tokens.push_back(std::move(token));
                token.clear();
                tokenStarted = false;
            }
        } else {
            token.push_back(static_cast<char>(character));
            tokenStarted = true;
        }
    }

    if (quote != 0) {
        error = "Unterminated quoted argument.";
        return false;
    }
    if (tokenStarted) {
        tokens.push_back(std::move(token));
    }
    if (tokens.empty()) {
        return true;
    }
    if (tokens.front().empty()) {
        error = "Command name cannot be empty.";
        return false;
    }

    command.name = std::move(tokens.front());
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        command.arguments.push_back(std::move(tokens[index]));
    }
    return true;
}

}
