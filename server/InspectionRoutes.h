#pragma once

#include <memory>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace jobq::server
{

    struct ServerContext;

    drogon::HttpResponsePtr workersResponse(const ServerContext &context, const drogon::HttpRequestPtr &request);
    drogon::HttpResponsePtr runtimeResponse(const ServerContext &context, const drogon::HttpRequestPtr &request);
    void registerInspectionRoutes(const std::shared_ptr<ServerContext> &context);

}
