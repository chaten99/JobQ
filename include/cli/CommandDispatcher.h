#pragma once

#include <functional>
#include <iosfwd>
#include <map>
#include <string>

#include "cli/InteractiveCommand.h"

namespace jobq::cli {

enum class CommandAction { Continue, Exit };

class CommandDispatcher {
public:
    using Handler = std::function<CommandAction(const InteractiveCommand&, std::ostream&)>;

    void registerCommand(std::string name, std::string usage, Handler handler);
    CommandAction dispatch(const InteractiveCommand& command, std::ostream& output) const;
    void printHelp(std::ostream& output) const;

private:
    struct Entry {
        std::string usage;
        Handler handler;
    };

    std::map<std::string, Entry> commands_;
};

}
