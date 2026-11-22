#pragma once

#include <atomic>
#include <functional>
#include <iosfwd>
#include <memory>

#include "ServerConfig.h"
#include "jobq/Engine.h"

namespace jobq::server
{

    struct ServerContext
    {
        explicit ServerContext(const ServerConfig &config);
        Engine engine;
        std::atomic<bool> listening{false};
        std::atomic<bool> stopping{false};
        bool ready() const;
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
