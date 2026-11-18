#pragma once

#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>

#include "cli/InteractiveCommand.h"

namespace jobq::cli {

enum class CommandAction { Continue, Exit };

class CommandDispatcher {
public:
    using Handler = std::function<CommandAction(const InteractiveCommand&, std::ostream&)>;

    void registerCommand(std::string name, std::string usage, Handler handler, std::string syntax = {});
    CommandAction dispatch(const InteractiveCommand& command, std::ostream& output) const;
    void printHelp(std::ostream& output, std::string_view command = {}) const;

private:
    struct Entry {
        std::string usage;
        std::string syntax;
        Handler handler;
    };

    std::map<std::string, Entry> commands_;
};

}
