
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace jobq::cli {

inline constexpr int kSuccessExitCode = 0;
inline constexpr int kCommandErrorExitCode = 1;
inline constexpr int kUsageErrorExitCode = 2;
inline constexpr int kNotImplementedExitCode = 3;

struct ParsedCommandLine {
    std::string command;

    std::vector<std::string> positionalArguments;

    std::unordered_map<std::string, std::string> options;

    bool helpRequested{false};
};

bool parseCommandLine(
    const std::vector<std::string>& arguments,
    ParsedCommandLine& parsed,
    std::string& errorMessage
);

std::string usageText();

}
