#include "InspectionRoutes.h"

#include "ApiJson.h"
#include "ServerRuntime.h"
#include "jobq/Version.h"

#include <drogon/drogon.h>

namespace jobq::server
{

    namespace
    {

        const char *workerState(const EngineSnapshot &snapshot)
        {
            if (snapshot.workersStopped)
                return "stopped";
            if (snapshot.workersStopping)
                return "stopping";
            return snapshot.workersStarted ? "running" : "created";
        }

        const char *engineState(EngineState state)
        {
            switch (state)
            {
            case EngineState::Created:
                return "created";
            case EngineState::Starting:
                return "starting";
            case EngineState::Running:
                return "running";
            case EngineState::Stopping:
                return "stopping";
            case EngineState::Stopped:
                return "stopped";
            }
            return "unavailable";
        }

        Json::Value workersJson(const EngineSnapshot &snapshot)
        {
            Json::Value data;
            data["state"] = workerState(snapshot);
            data["configured_workers"] = Json::UInt64(snapshot.workerCount);
            return data;
        }

        Json::Value runtimeJson(const EngineSnapshot &snapshot, std::chrono::seconds uptime)
        {
            Json::Value data;
            data["server"]["state"] = "running";
            data["server"]["uptime_seconds"] = Json::UInt64(uptime.count());
            data["engine"]["state"] = engineState(snapshot.state);
            data["engine"]["accepting_jobs"] = true;
            data["engine"]["configured_workers"] = Json::UInt64(snapshot.workerCount);
            data["version"]["jobq"] = std::string(version::string);
            return data;
        }

        template <typename Serialize>
        drogon::HttpResponsePtr inspect(const ServerContext &context, const drogon::HttpRequestPtr &request, Serialize serialize)
        {
            try
            {
                if (!request->query().empty() || !request->getParameters().empty())
                {
                    return apiError(drogon::k400BadRequest, "INVALID_REQUEST", "Inspection accepts no query parameters.");
                }
                if (context.ready())
                {
                    const auto snapshot = context.engine.snapshot();
                    if (snapshot.state == EngineState::Running && context.listening.load() && !context.stopping.load())
                    {
                        return apiData(serialize(snapshot));
                    }
                }
                return apiError(drogon::k503ServiceUnavailable, "ENGINE_UNAVAILABLE", "The processing engine is not ready or is shutting down.");
            }
            catch (...)
            {
                return apiError(drogon::k500InternalServerError, "INTERNAL_ERROR", "The request could not be completed.");
            }
        }

    }

    drogon::HttpResponsePtr workersResponse(const ServerContext &context, const drogon::HttpRequestPtr &request)
    {
        return inspect(context, request, workersJson);
    }

    drogon::HttpResponsePtr runtimeResponse(const ServerContext &context, const drogon::HttpRequestPtr &request)
    {
        return inspect(context, request, [&](const EngineSnapshot &snapshot)
                       { return runtimeJson(snapshot, context.uptime()); });
    }

    void registerInspectionRoutes(const std::shared_ptr<ServerContext> &context)
    {
        using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
        drogon::app().registerHandler("/api/v1/workers",
                                      [context](const drogon::HttpRequestPtr &request, Callback &&callback)
                                      {
                                          callback(workersResponse(*context, request));
                                      },
                                      {drogon::Get});
        drogon::app().registerHandler("/api/v1/runtime",
                                      [context](const drogon::HttpRequestPtr &request, Callback &&callback)
                                      {
                                          callback(runtimeResponse(*context, request));
                                      },
                                      {drogon::Get});
    }

}
