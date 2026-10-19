
#include "cli/CommandLine.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(
        argc > 1 ? static_cast<std::size_t>(argc - 1) : 0
    );

    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    jobq::cli::ParsedCommandLine parsed;
    std::string errorMessage;

    if (!jobq::cli::parseCommandLine(
            arguments,
            parsed,
            errorMessage
        )) {

        std::cerr
            << "Error: "
            << errorMessage
            << "\n\n"
            << jobq::cli::usageText();

        return jobq::cli::kUsageErrorExitCode;
    }

    if (parsed.helpRequested) {
        std::cout << jobq::cli::usageText();

        return jobq::cli::kSuccessExitCode;
    }

    std::cerr
        << "Command '"
        << parsed.command
        << "' is recognized but not implemented yet.\n";

    return jobq::cli::kNotImplementedExitCode;
}
