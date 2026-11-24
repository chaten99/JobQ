#include "ApiJson.h"
#include <string_view>
#include <utility>

namespace jobq::server
{

    drogon::HttpResponsePtr apiError(drogon::HttpStatusCode status, const std::string &code, const std::string &message)
    {
        Json::Value body;
        body["error"]["code"] = code;
        body["error"]["message"] = message;
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(status);
        response->addHeader("Cache-Control", "no-store");
        return response;
    }

    drogon::HttpResponsePtr apiData(Json::Value data, drogon::HttpStatusCode status)
    {
        Json::Value body;
        body["data"] = std::move(data);
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(status);
        response->addHeader("Cache-Control", "no-store");
        return response;
    }

    Json::Value jobJson(const JobSnapshot &job)
    {
        constexpr std::size_t previewLimit = 160;
        constexpr char hex[] = "0123456789ABCDEF";
        std::string preview;
        for (const unsigned char byte : std::string_view(job.payload).substr(0, previewLimit))
        {
            if (byte >= 0x20 && byte < 0x7f && byte != '\\')
                preview += static_cast<char>(byte);
            else
            {
                preview += "\\x";
                preview += hex[byte >> 4];
                preview += hex[byte & 15];
            }
        }
        Json::Value data;
        data["id"] = std::to_string(job.id);
        data["type"] = job.type.substr(0, 256);
        data["type_truncated"] = job.type.size() > 256;
        data["status"] = std::string(toString(job.status));
        data["priority"] = std::string(toString(job.priority));
        data["attempts_made"] = Json::UInt64(job.attemptsMade);
        data["retry"]["max_retries"] = Json::UInt64(job.retryPolicy.maxRetries);
        data["retry"]["initial_delay_ms"] = Json::Int64(job.retryPolicy.initialDelay.count());
        data["retry"]["max_delay_ms"] = Json::Int64(job.retryPolicy.maxDelay.count());
        data["retry"]["multiplier"] = job.retryPolicy.multiplier;
        data["payload_preview"] = preview;
        data["payload_preview_encoding"] = "escaped_bytes";
        data["payload_bytes"] = Json::UInt64(job.payload.size());
        data["payload_truncated"] = job.payload.size() > previewLimit;
        return data;
    }

}
