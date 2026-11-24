#include "MetricsRoutes.h"

#include "ApiJson.h"
#include "ServerRuntime.h"

#include <cmath>
#include <drogon/drogon.h>

namespace jobq::server {

drogon::HttpResponsePtr metricsResponse(const ServerContext& context, const drogon::HttpRequestPtr& request) {
    try {
        if (!request->query().empty() || !request->getParameters().empty()) {
            return apiError(drogon::k400BadRequest, "INVALID_REQUEST", "Metrics accepts no query parameters.");
        }
        if (!context.ready()) {
            return apiError(drogon::k503ServiceUnavailable, "ENGINE_UNAVAILABLE", "The processing engine is not ready or is shutting down.");
        }
        const auto snapshot = context.engine.metrics();
        Json::Value data;
        data["schema_version"] = 1;
        auto& counters = data["counters"];
        counters["submitted_jobs"] = std::to_string(snapshot.submittedJobs);
        counters["started_executions"] = std::to_string(snapshot.startedExecutions);
        counters["completed_executions"] = std::to_string(snapshot.completedExecutions);
        counters["failed_executions"] = std::to_string(snapshot.failedExecutions);
        counters["scheduled_retries"] = std::to_string(snapshot.scheduledRetries);
        counters["finished_executions"] = std::to_string(snapshot.finishedExecutions);
        auto& performance = data["performance"];
        performance["total_execution_time_ns"] = std::to_string(snapshot.totalExecutionTime.count());
        performance["average_execution_time_ns"] = std::to_string(snapshot.averageExecutionTime.count());
        performance["elapsed_time_ns"] = std::to_string(snapshot.elapsedTime.count());
        performance["finished_executions_per_second"] = std::isfinite(snapshot.throughputJobsPerSecond)
            ? Json::Value(snapshot.throughputJobsPerSecond) : Json::Value(Json::nullValue);
        return apiData(std::move(data));
    } catch (...) {
        return apiError(drogon::k500InternalServerError, "INTERNAL_ERROR", "The request could not be completed.");
    }
}

void registerMetricsRoutes(const std::shared_ptr<ServerContext>& context) {
    drogon::app().registerHandler("/api/v1/metrics",
        [context](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(metricsResponse(*context, request));
        }, {drogon::Get});
}

}
