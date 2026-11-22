#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>

#include "ServerConfig.h"
#include "jobq/Engine.h"

namespace jobq::server
{

    struct ServerContext
    {
        explicit ServerContext(const ServerConfig &config);
        Engine engine;
        void markListening();
        std::chrono::seconds uptime() const;
        std::atomic<bool> listening{false};
        std::atomic<bool> stopping{false};
        bool ready() const;
        JobId submit(std::string type, std::string payload, JobOptions options = {});
        void requestStop();

    private:
        std::mutex admission_;
        std::atomic<std::chrono::steady_clock::duration::rep> readyTicks_{0};
    };

    class ServerRuntime
    {
    public:
        explicit ServerRuntime(ServerConfig config, std::function<void(Engine &)> initialize = {});
        int run(std::ostream &output, std::ostream &errors);
        void requestStop() noexcept;
        bool ready() const;

    private:
        ServerConfig config_;
        std::function<void(Engine &)> initialize_;
        std::shared_ptr<ServerContext> context_;
        std::atomic<bool> used_{false};
    };

}
