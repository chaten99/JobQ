
#pragma once

#include <iosfwd>

#include "cli/CommandLine.h"

namespace jobq::cli {

int executeSubmitCommand(
    const ParsedCommandLine& parsed,
    std::ostream& output,
    std::ostream& errorOutput
);

}
