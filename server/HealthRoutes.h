#pragma once

#include <memory>
#include <drogon/HttpResponse.h>

namespace jobq::server
{
    struct ServerContext;
    drogon::HttpResponsePtr readinessResponse(const ServerContext &context);
    void registerHealthRoutes(const std::shared_ptr<ServerContext> &context);
}
