#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace jobq::cli {

struct InteractiveCommand {
    std::string name;
    std::vector<std::string> arguments;
};

bool parseInteractiveCommand(
    std::string_view input,
    InteractiveCommand& command,
    std::string& error
);

}
