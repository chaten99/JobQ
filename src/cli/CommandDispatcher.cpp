#include "cli/CommandDispatcher.h"

#include <cctype>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace jobq::cli {

void CommandDispatcher::registerCommand(std::string name, std::string usage, Handler handler) {
    if (name.empty() || usage.empty() || !handler) {
        throw std::invalid_argument("Command name, usage, and handler must be non-empty");
    }
    for (const unsigned char character : name) {
        if (!std::isalnum(character) && character != '-' && character != '_') {
            throw std::invalid_argument("Command names require letters, numbers, '-' or '_'");
        }
    }
    const auto [entry, inserted] = commands_.try_emplace(
        std::move(name), Entry{std::move(usage), std::move(handler)}
    );
    if (!inserted) {
        throw std::invalid_argument("Command already registered: " + entry->first);
    }
}

CommandAction CommandDispatcher::dispatch(
    const InteractiveCommand& command,
    std::ostream& output
) const {
    if (command.name.empty()) {
        return CommandAction::Continue;
    }
    const auto entry = commands_.find(command.name);
    if (entry == commands_.end()) {
        throw std::invalid_argument("Unknown command: " + command.name + ". Use help for available commands.");
    }
    return entry->second.handler(command, output);
}

void CommandDispatcher::printHelp(std::ostream& output) const {
    output << "Available commands:\n";
    for (const auto& [name, entry] : commands_) {
        output << "  " << name << "  " << entry.usage << '\n';
    }
}

}
