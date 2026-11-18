#include "cli/CommandDispatcher.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace jobq::cli {

void CommandDispatcher::registerCommand(std::string name, std::string usage, Handler handler, std::string syntax) {
    if (name.empty() || usage.empty() || !handler) {
        throw std::invalid_argument("Command name, usage, and handler must be non-empty");
    }
    for (const unsigned char character : name) {
        if (!std::isalnum(character) && character != '-' && character != '_') {
            throw std::invalid_argument("Command names require letters, numbers, '-' or '_'");
        }
    }
    if (syntax.empty()) {
        syntax = name;
    }
    const auto [entry, inserted] = commands_.try_emplace(
        std::move(name), Entry{std::move(usage), std::move(syntax), std::move(handler)}
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

void CommandDispatcher::printHelp(std::ostream& output, std::string_view command) const {
    if (!command.empty()) {
        const auto entry = commands_.find(std::string(command));
        if (entry == commands_.end()) {
            throw std::invalid_argument("Unknown help topic: " + std::string(command) + ". Usage: help [command]");
        }
        output << "Usage: " << entry->second.syntax << '\n' << entry->second.usage << '\n';
        return;
    }
    std::size_t width = 0;
    for (const auto& [name, entry] : commands_) {
        width = std::max(width, entry.syntax.size());
    }
    std::ostringstream report;
    report << "Available commands:\n";
    for (const auto& [name, entry] : commands_) {
        report << "  " << std::left << std::setw(static_cast<int>(width + 2))
               << entry.syntax << entry.usage << '\n';
    }
    output << report.str();
}

}
