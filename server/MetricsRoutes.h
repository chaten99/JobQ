#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace jobq::server {

struct ServerContext;
drogon::HttpResponsePtr metricsResponse(const ServerContext& context, const drogon::HttpRequestPtr& request);
void registerMetricsRoutes(const std::shared_ptr<ServerContext>& context);

}
