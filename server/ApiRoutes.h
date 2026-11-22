#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <memory>

namespace jobq::server
{
    struct ServerContext;
    drogon::HttpResponsePtr listJobsResponse(ServerContext &context, const drogon::HttpRequestPtr &request);
    drogon::HttpResponsePtr submitJobResponse(ServerContext &context, const drogon::HttpRequestPtr &request);
    drogon::HttpResponsePtr getJobResponse(ServerContext &context, const std::string &id);
    drogon::HttpResponsePtr queueResponse(ServerContext &context);
    void registerApiRoutes(const std::shared_ptr<ServerContext> &context);
}
