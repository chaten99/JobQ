#include "ServerConfig.h"
#include "ServerRuntime.h"

#include <exception>
#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    jobq::server::ServerConfig config;
    try
    {
        config = jobq::server::parseConfig(std::vector<std::string>(argv + 1, argv + argc));
    }
    catch (const std::exception &error)
    {
        std::cerr << "Configuration error: " << error.what() << '\n'
                  << jobq::server::usage();
        return 2;
    }
    if (config.help)
    {
        std::cout << jobq::server::usage();
        return 0;
    }
    try
    {
        jobq::server::ServerRuntime runtime(config);
        return runtime.run(std::cout, std::cerr);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Server startup failure: " << error.what() << '\n';
        return 1;
    }
}
