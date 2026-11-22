#include "cli/CommandHandlers.h"
#include "cli/CommandLine.h"
#include "cli/InteractiveRuntime.h"

#include <iostream>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "interactive") {
        if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
            std::cout << "Usage: jobq_cli interactive\nStart a persistent session. Use help to list its commands.\n";
            return jobq::cli::kSuccessExitCode;
        }
        if (argc != 2) {
            std::cerr << "Error: interactive does not accept arguments.\n";
            return jobq::cli::kUsageErrorExitCode;
        }
        try {
            jobq::cli::InteractiveRuntime runtime;
            return runtime.runTerminal(std::cout, std::cerr);
        } catch (const std::exception& error) {
            std::cerr << "Unable to initialize interactive CLI: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Unable to initialize interactive CLI: unknown error.\n";
        }
        return jobq::cli::kCommandErrorExitCode;
    }

    std::vector<std::string> arguments;

    arguments.reserve(
        argc > 1
            ? static_cast<std::size_t>(argc - 1)
            : 0
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

    if (parsed.command == "submit") {
        return jobq::cli::executeSubmitCommand(
            parsed,
            std::cout,
            std::cerr
        );
    }

    if (parsed.command == "list") {
        return jobq::cli::executeListCommand(
            parsed,
            std::cout,
            std::cerr
        );
    }

    if (parsed.command == "status") {
        return jobq::cli::executeStatusCommand(
            parsed,
            std::cout,
            std::cerr
        );
    }

    if (parsed.command == "run") {
        return jobq::cli::executeRunCommand(
            parsed,
            std::cout,
            std::cerr
        );
    }

    if (parsed.command == "metrics") {
        return jobq::cli::executeMetricsCommand(
            parsed,
            std::cout,
            std::cerr
        );
    }

    std::cerr
        << "Command '"
        << parsed.command
        << "' is recognized but not implemented yet.\n";

    return jobq::cli::kNotImplementedExitCode;
}
