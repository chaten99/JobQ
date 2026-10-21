#pragma once

#include <iosfwd>

#include "cli/CommandLine.h"

namespace jobq::cli {

int executeSubmitCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeListCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeStatusCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

int executeRunCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

} // namespace jobq::cli
