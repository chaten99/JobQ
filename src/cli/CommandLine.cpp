#include "cli/CommandLine.h"

#include <string>
#include <string_view>

namespace jobq::cli {

namespace {

bool isSupportedCommand(std::string_view command) {
    return command == "submit" ||
           command == "list" ||
           command == "status" ||
           command == "run";
}

bool isAllowedOption(
    std::string_view command,
    std::string_view option
) {
    if (option == "storage") {
        return true;
    }

    if (command == "submit") {
        return option == "type" ||
               option == "payload" ||
               option == "priority" ||
               option == "retries";
    }

    if (command == "list") {
        return option == "status" ||
               option == "limit";
    }

    if (command == "run") {
        return option == "workers";
    }

    return false;
}

bool requestsHelp(const std::vector<std::string>& arguments) {
    for (const auto& argument : arguments) {
        if (argument == "--help" || argument == "-h") {
            return true;
        }
    }

    return false;
}

} // namespace

bool parseCommandLine(
    const std::vector<std::string>& arguments,
    ParsedCommandLine& parsed,
    std::string& errorMessage
) {
    parsed = ParsedCommandLine{};
    errorMessage.clear();

    if (requestsHelp(arguments)) {
        parsed.helpRequested = true;
        return true;
    }

    if (arguments.empty()) {
        errorMessage =
            "Missing command. Use --help to see available commands.";
        return false;
    }

    parsed.command = arguments.front();

    if (!isSupportedCommand(parsed.command)) {
        errorMessage = "Unknown command: " + parsed.command;
        return false;
    }

    for (std::size_t index = 1;
         index < arguments.size();
         ++index) {

        const std::string& token = arguments[index];

        if (token.rfind("--", 0) == 0) {
            const auto equalsPosition = token.find('=');

            std::string optionName;
            std::string optionValue;

            if (equalsPosition == std::string::npos) {
                optionName = token.substr(2);

                if (optionName.empty()) {
                    errorMessage = "Invalid empty option name.";
                    return false;
                }

                if (!isAllowedOption(
                        parsed.command,
                        optionName
                    )) {
                    errorMessage =
                        "Unknown option for command '" +
                        parsed.command + "': --" + optionName;
                    return false;
                }

                if (index + 1 >= arguments.size() ||
                    arguments[index + 1].rfind("--", 0) == 0) {
                    errorMessage =
                        "Missing value for option --" + optionName;
                    return false;
                }

                optionValue = arguments[++index];
            } else {
                optionName = token.substr(
                    2,
                    equalsPosition - 2
                );

                optionValue = token.substr(equalsPosition + 1);

                if (optionName.empty()) {
                    errorMessage = "Invalid empty option name.";
                    return false;
                }

                if (!isAllowedOption(
                        parsed.command,
                        optionName
                    )) {
                    errorMessage =
                        "Unknown option for command '" +
                        parsed.command + "': --" + optionName;
                    return false;
                }
            }

            const auto [iterator, inserted] =
                parsed.options.emplace(
                    optionName,
                    optionValue
                );

            if (!inserted) {
                errorMessage =
                    "Duplicate option: --" + optionName;
                return false;
            }

            (void)iterator;
            continue;
        }

        if (!token.empty() && token.front() == '-') {
            errorMessage =
                "Unknown short option: " + token +
                ". Only -h is supported.";
            return false;
        }

        parsed.positionalArguments.push_back(token);
    }

    return true;
}

std::string usageText() {
    return
        "JobQ Command-Line Interface\n"
        "\n"
        "Usage:\n"
        "  jobq_cli <command> [options] [arguments]\n"
        "\n"
        "Commands:\n"
        "  submit --type TYPE --payload PAYLOAD "
        "[--priority PRIORITY] [--retries COUNT] [--storage PATH]\n"
        "      Persist a new job.\n"
        "\n"
        "  list [--status STATUS] [--limit COUNT] [--storage PATH]\n"
        "      List stored jobs.\n"
        "\n"
        "  status JOB_ID [--storage PATH]\n"
        "      Inspect a job by ID.\n"
        "\n"
        "  run [--workers COUNT] [--storage PATH]\n"
        "      Start job workers (implemented in a later commit).\n"
        "\n"
        "General options:\n"
        "  -h, --help    Display this help.\n";
}

}
