#pragma once

#include <memory>

namespace jobq::server
{

    struct ServerContext;
    void registerEventRoutes(const std::shared_ptr<ServerContext> &context);

}
