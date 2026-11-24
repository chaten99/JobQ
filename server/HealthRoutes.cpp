#include "HealthRoutes.h"
#include "ServerRuntime.h"

#include <drogon/drogon.h>

namespace jobq::server
{

    drogon::HttpResponsePtr readinessResponse(const ServerContext &context)
    {
        const bool ready = context.ready() && drogon::app().isRunning();
        Json::Value body;
        body["status"] = ready ? "ready" : "not_ready";
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(ready ? drogon::k200OK : drogon::k503ServiceUnavailable);
        return response;
    }

    void registerHealthRoutes(const std::shared_ptr<ServerContext> &context)
    {
        drogon::app().registerHandler("/health/live", [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback)
                                      {
        Json::Value body;
        body["status"] = "ok";
        callback(drogon::HttpResponse::newHttpJsonResponse(body)); }, {drogon::Get});
        drogon::app().registerHandler("/health/ready", [context](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback)
                                      {
        callback(readinessResponse(*context)); }, {drogon::Get});
    }

}
