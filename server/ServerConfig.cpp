#include "ServerConfig.h"

#include <charconv>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#include <trantor/net/InetAddress.h>

namespace jobq::server
{

    void validateConfig(const ServerConfig &config)
    {
        const trantor::InetAddress address(config.address, config.port, config.address.find(':') != std::string::npos);
        if (config.address.empty() || address.isUnspecified())
            throw std::invalid_argument("Bind address must be a numeric IPv4 or IPv6 address");
        if (config.httpThreads < 1 || config.httpThreads > 64)
            throw std::invalid_argument("HTTP threads must be 1..64");
        if (config.engineWorkers < 1 || config.engineWorkers > 256)
            throw std::invalid_argument("Engine workers must be 1..256");
        if (config.maxBodyBytes < 1 || config.maxBodyBytes > 16 * 1024 * 1024)
            throw std::invalid_argument("Body limit must be 1..16777216 bytes");
        if (config.maxConnections < 1 || config.maxConnections > 65536)
            throw std::invalid_argument("Connection limit must be 1..65536");
    }

    ServerConfig parseConfig(const std::vector<std::string> &arguments)
    {
        ServerConfig config;
        std::unordered_set<std::string> seen;
        for (std::size_t index = 0; index < arguments.size(); ++index)
        {
            const auto &option = arguments[index];
            if (option == "--help" || option == "-h")
            {
                if (arguments.size() != 1)
                    throw std::invalid_argument("Help accepts no other arguments");
                config.help = true;
                return config;
            }
            if (option != "--bind" && option != "--port" && option != "--http-threads" &&
                option != "--engine-workers" && option != "--max-body-bytes" && option != "--max-connections")
            {
                throw std::invalid_argument("Unsupported server argument: " + option);
            }
            if (!seen.insert(option).second)
                throw std::invalid_argument("Repeated server argument: " + option);
            if (++index == arguments.size())
                throw std::invalid_argument("Missing value for " + option);
            const auto &text = arguments[index];
            if (option == "--bind")
            {
                config.address = text;
                continue;
            }
            std::uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw std::invalid_argument("Invalid number for " + option);
            if (option == "--port")
            {
                if (value > 65535)
                    throw std::invalid_argument("Port must be 0..65535; 0 selects an ephemeral port");
                config.port = static_cast<std::uint16_t>(value);
            }
            else
            {
                if (value > 16777216)
                    throw std::invalid_argument("Value out of range for " + option);
                if (option == "--http-threads")
                    config.httpThreads = static_cast<std::size_t>(value);
                if (option == "--engine-workers")
                    config.engineWorkers = static_cast<std::size_t>(value);
                if (option == "--max-body-bytes")
                    config.maxBodyBytes = static_cast<std::size_t>(value);
                if (option == "--max-connections")
                    config.maxConnections = static_cast<std::size_t>(value);
            }
        }
        validateConfig(config);
        return config;
    }

    std::string usage()
    {
        return "Usage: jobq_server [--bind IP] [--port 0..65535] [--http-threads 1..64]\n"
               "                   [--engine-workers 1..256] [--max-body-bytes 1..16777216]\n"
               "                   [--max-connections 1..65536]\n"
               "Defaults: 127.0.0.1:8080, 2 HTTP threads, 4 Engine workers, 1048576 body bytes, 1024 connections.\n";
    }

}
