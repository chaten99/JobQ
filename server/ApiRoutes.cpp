#include "ApiRoutes.h"
#include "ApiJson.h"
#include "ServerRuntime.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <drogon/drogon.h>

namespace jobq::server
{

    namespace
    {

        struct InvalidRequest : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        std::uint64_t number(std::string_view text)
        {
            std::uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw InvalidRequest("Expected an unsigned decimal integer.");
            return value;
        }

        void fields(const Json::Value &object, std::initializer_list<std::string_view> allowed)
        {
            if (!object.isObject())
                throw InvalidRequest("Expected a JSON object.");
            for (const auto &name : object.getMemberNames())
            {
                if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                    throw InvalidRequest("Unsupported request field.");
            }
        }

        std::uint64_t integer(const Json::Value &value, std::uint64_t maximum)
        {
            if (!value.isUInt64() || value.type() == Json::realValue || value.asUInt64() > maximum)
            {
                throw InvalidRequest("Job option integer is outside its supported range.");
            }
            return value.asUInt64();
        }

        bool available(const ServerContext &context) { return context.ready(); }
        drogon::HttpResponsePtr unavailable()
        {
            return apiError(drogon::k503ServiceUnavailable, "ENGINE_UNAVAILABLE", "The processing engine is not ready or is shutting down.");
        }

        template <typename Operation>
        drogon::HttpResponsePtr guarded(Operation &&operation)
        {
            try
            {
                return operation();
            }
            catch (const InvalidRequest &error)
            {
                return apiError(drogon::k400BadRequest, "INVALID_REQUEST", error.what());
            }
            catch (...)
            {
                return apiError(drogon::k500InternalServerError, "INTERNAL_ERROR", "The request could not be completed.");
            }
        }

        std::string mediaType(const drogon::HttpRequestPtr &request)
        {
            auto type = request->getHeader("content-type");
            type = type.substr(0, type.find(';'));
            const auto first = type.find_first_not_of(" \t");
            if (first == std::string::npos)
                return {};
            type = type.substr(first, type.find_last_not_of(" \t") - first + 1);
            std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return type;
        }

    }

    drogon::HttpResponsePtr listJobsResponse(ServerContext &context, const drogon::HttpRequestPtr &request)
    {
        return guarded([&]
                       {
        if (!available(context)) return unavailable();
        std::size_t count = 0;
        for (const auto c : request->query()) if (c == '&') ++count;
        const auto& parameters = request->getParameters();
        if (!request->query().empty() && count + 1 != parameters.size()) throw InvalidRequest("Repeated or empty query parameters are not supported.");
        for (const auto& [key, value] : parameters) {
            if (key != "limit" && key != "offset" && key != "status" && key != "type") throw InvalidRequest("Unsupported query parameter.");
        }
        const auto limit = parameters.contains("limit") ? number(parameters.at("limit")) : 50;
        const auto offset = parameters.contains("offset") ? number(parameters.at("offset")) : 0;
        if (limit < 1 || limit > 100 || offset > std::numeric_limits<std::size_t>::max()) throw InvalidRequest("Limit must be 1..100 and offset must fit the platform index.");
        const auto status = parameters.contains("status") ? parameters.at("status") : "";
        const auto type = parameters.contains("type") ? parameters.at("type") : "";
        if (parameters.contains("status") && status != "PENDING" && status != "RUNNING" && status != "COMPLETED" &&
            status != "FAILED" && status != "CANCELLED") throw InvalidRequest("Unsupported status filter.");
        if (parameters.contains("type") && (type.empty() || type.size() > 256)) throw InvalidRequest("Type filter must contain 1..256 bytes.");
        auto jobs = context.engine.listJobs();
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const auto& job) {
            return (!status.empty() && toString(job.status) != status) || (!type.empty() && job.type != type);
        }), jobs.end());
        Json::Value body;
        body["data"] = Json::Value(Json::arrayValue);
        const auto begin = std::min<std::uint64_t>(offset, jobs.size());
        const auto end = begin + std::min<std::uint64_t>(limit, jobs.size() - begin);
        for (auto index = begin; index < end; ++index) body["data"].append(jobJson(jobs[static_cast<std::size_t>(index)]));
        body["pagination"]["limit"] = Json::UInt64(limit);
        body["pagination"]["offset"] = Json::UInt64(offset);
        body["pagination"]["total"] = Json::UInt64(jobs.size());
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->addHeader("Cache-Control", "no-store");
        return response; });
    }

    drogon::HttpResponsePtr submitJobResponse(ServerContext &context, const drogon::HttpRequestPtr &request)
    {
        return guarded([&]
                       {
        if (!available(context)) return unavailable();
        if (mediaType(request) != "application/json") {
            return apiError(drogon::k415UnsupportedMediaType, "UNSUPPORTED_MEDIA_TYPE", "Use application/json.");
        }
        if (!request->query().empty()) throw InvalidRequest("Submission accepts no query parameters.");
        Json::CharReaderBuilder builder;
        builder["allowComments"] = false;
        builder["collectComments"] = false;
        builder["allowTrailingCommas"] = false;
        builder["rejectDupKeys"] = true;
        builder["failIfExtra"] = true;
        builder["stackLimit"] = 64;
        Json::Value body;
        std::string error;
        const auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
        const auto bytes = request->body();
        bool valid = false;
        try { valid = !bytes.empty() && reader->parse(bytes.data(), bytes.data() + bytes.size(), &body, &error); }
        catch (const Json::Exception&) { }
        if (!valid) {
            return apiError(drogon::k400BadRequest, "INVALID_JSON", "A valid JSON request body is required.");
        }
        fields(body, {"type", "payload", "priority", "retry"});
        if (!body["type"].isString() || body["type"].asString().empty() || body["type"].asString().size() > 256 ||
            !body["payload"].isString()) throw InvalidRequest("Type must contain 1..256 bytes and payload must be a string.");
        JobOptions options;
        if (body.isMember("priority")) {
            if (!body["priority"].isString()) throw InvalidRequest("Priority must be LOW, NORMAL, or HIGH.");
            const auto priority = body["priority"].asString();
            if (priority == "LOW") options.priority = JobPriority::Low;
            else if (priority == "NORMAL") options.priority = JobPriority::Normal;
            else if (priority == "HIGH") options.priority = JobPriority::High;
            else throw InvalidRequest("Priority must be LOW, NORMAL, or HIGH.");
        }
        if (body.isMember("retry")) {
            const auto& retry = body["retry"];
            fields(retry, {"max_retries", "initial_delay_ms", "max_delay_ms", "multiplier"});
            if (retry.isMember("max_retries")) options.retryPolicy.maxRetries = integer(retry["max_retries"], 10);
            if (retry.isMember("initial_delay_ms")) options.retryPolicy.initialDelay = std::chrono::milliseconds(integer(retry["initial_delay_ms"], 60000));
            if (retry.isMember("max_delay_ms")) options.retryPolicy.maxDelay = std::chrono::milliseconds(integer(retry["max_delay_ms"], 60000));
            if (retry.isMember("multiplier")) {
                if (!retry["multiplier"].isNumeric()) throw InvalidRequest("Retry multiplier must be numeric.");
                const auto multiplier = retry["multiplier"].asDouble();
                if (!std::isfinite(multiplier) || multiplier < 1 || multiplier > 10) throw InvalidRequest("Retry multiplier must be 1..10.");
                options.retryPolicy.multiplier = multiplier;
            }
        }
        try {
            const auto id = context.submit(body["type"].asString(), body["payload"].asString(), options);
            const auto job = context.engine.getJob(id);
            Json::Value data;
            if (job) data = jobJson(*job);
            else data["id"] = std::to_string(id);
            auto response = apiData(std::move(data), drogon::k201Created);
            response->addHeader("Location", "/api/v1/jobs/" + std::to_string(id));
            return response;
        } catch (const std::invalid_argument&) {
            return apiError(drogon::k422UnprocessableEntity, "UNKNOWN_JOB_TYPE", "No handler is registered for the requested job type.");
        } catch (const std::logic_error&) { return unavailable(); } });
    }

    drogon::HttpResponsePtr getJobResponse(ServerContext &context, const std::string &idText)
    {
        return guarded([&]
                       {
        if (!available(context)) return unavailable();
        const auto id = number(idText);
        if (!id) throw InvalidRequest("Job ID must be a positive decimal uint64.");
        const auto job = context.engine.getJob(id);
        if (!job) return apiError(drogon::k404NotFound, "JOB_NOT_FOUND", "The requested job does not exist.");
        return apiData(jobJson(*job)); });
    }

    drogon::HttpResponsePtr queueResponse(ServerContext &context)
    {
        return guarded([&]
                       {
        if (!available(context)) return unavailable();
        const auto snapshot = context.engine.snapshot();
        Json::Value data;
        data["ready"] = Json::UInt64(snapshot.readyJobs);
        data["delayed"] = Json::UInt64(snapshot.delayedJobs);
        data["closed"] = snapshot.queueClosed;
        return apiData(std::move(data)); });
    }

    void registerApiRoutes(const std::shared_ptr<ServerContext> &context)
    {
        drogon::app().registerHandler("/api/v1/jobs", [context](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback)
                                      { callback(listJobsResponse(*context, request)); }, {drogon::Get});
        drogon::app().registerHandler("/api/v1/jobs", [context](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback)
                                      { callback(submitJobResponse(*context, request)); }, {drogon::Post});
        drogon::app().registerHandler("/api/v1/jobs/{1}", [context](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback, const std::string &id)
                                      { callback(request->query().empty() ? getJobResponse(*context, id) :
                                          apiError(drogon::k400BadRequest, "INVALID_REQUEST", "Lookup accepts no query parameters.")); }, {drogon::Get});
        drogon::app().registerHandler("/api/v1/queue", [context](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback)
                                      { callback(request->query().empty() ? queueResponse(*context) :
                                          apiError(drogon::k400BadRequest, "INVALID_REQUEST", "Queue accepts no query parameters.")); }, {drogon::Get});
        drogon::app().setCustomErrorHandler([](drogon::HttpStatusCode status, const drogon::HttpRequestPtr &request)
                                            {
        const auto code = status == drogon::k413RequestEntityTooLarge ? "PAYLOAD_TOO_LARGE" :
                          status == drogon::k405MethodNotAllowed ? "METHOD_NOT_ALLOWED" :
                          status == drogon::k404NotFound ? "RESOURCE_NOT_FOUND" :
                          status == drogon::k500InternalServerError ? "INTERNAL_ERROR" :
                          status == drogon::k400BadRequest ? "INVALID_REQUEST" : "HTTP_ERROR";
        auto response = apiError(status, code, "The HTTP request could not be handled.");
        if (status == drogon::k405MethodNotAllowed) response->addHeader("Allow",
            request && request->path() == "/api/v1/jobs" ? "GET, POST" : "GET");
        return response; });
    }

}
