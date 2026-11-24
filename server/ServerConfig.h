#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jobq::server
{

    struct ServerConfig
    {
        std::string address{"127.0.0.1"};
        std::uint16_t port{8080};
        std::size_t httpThreads{2};
        std::size_t engineWorkers{4};
        std::size_t maxBodyBytes{1024 * 1024};
        std::size_t maxConnections{1024};
        bool help{false};
        std::filesystem::path storagePath{};
    };

    ServerConfig parseConfig(const std::vector<std::string> &arguments);
    void validateConfig(const ServerConfig &config);
    std::string usage();

}
