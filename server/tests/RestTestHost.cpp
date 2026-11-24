#include "ServerConfig.h"
#include "ServerRuntime.h"
#include <iostream>

int main(int argc, char** argv) {
    try {
        const auto config = jobq::server::parseConfig(std::vector<std::string>(argv + 1, argv + argc));
        jobq::server::ServerRuntime runtime(config, [](jobq::Engine& engine) {
            engine.registerHandler("ECHO", [](const jobq::Job&) {});
        });
        return runtime.run(std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
