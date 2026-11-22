#include "ApiRoutes.h"
#include "ServerRuntime.h"

#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>

#include "helpers/TestSupport.h"
#include "helpers/TemporaryDirectory.h"

namespace
{

      using jobq_test::check;
      using namespace std::chrono_literals;

      drogon::HttpRequestPtr request(const std::string &query = "", const std::string &body = "")
      {
            auto value = drogon::HttpRequest::newHttpRequest();
            value->setPath("/api/v1/jobs");
            std::istringstream pairs(query);
            std::string pair;
            while (std::getline(pairs, pair, '&'))
            {
                  const auto equal = pair.find('=');
                  value->setParameter(pair.substr(0, equal), equal == std::string::npos ? "" : pair.substr(equal + 1));
            }
            value->setBody(body);
            value->addHeader("content-type", "application/json");
            return value;
      }

      Json::Value json(const drogon::HttpResponsePtr &response)
      {
            const auto value = response->getJsonObject();
            if (!value)
                  throw std::runtime_error("Response is not JSON");
            return *value;
      }

      std::string body(const std::string &type, const std::string &payload)
      {
            Json::Value value;
            value["type"] = type;
            value["payload"] = payload;
            return Json::writeString(Json::StreamWriterBuilder{}, value);
      }

      void validationAndJobs()
      {
            jobq::server::ServerConfig config;
            config.engineWorkers = 1;
            jobq::server::ServerContext context(config);
            check(jobq::server::listJobsResponse(context, request())->statusCode() == drogon::k503ServiceUnavailable &&
                      jobq::server::submitJobResponse(context, request("", body("TASK", "")))->statusCode() == drogon::k503ServiceUnavailable &&
                      jobq::server::getJobResponse(context, "1")->statusCode() == drogon::k503ServiceUnavailable &&
                      jobq::server::queueResponse(context)->statusCode() == drogon::k503ServiceUnavailable,
                  "Every REST operation rejects an unavailable Engine");
            jobq_test::Counter entered;
            jobq_test::Gate release;
            std::string received;
            context.engine.registerHandler("BLOCK", [&](const jobq::Job &job)
                                           {
        received = job.getPayload();
        entered.add();
        if (!release.wait()) throw std::runtime_error("Worker release timed out"); });
            context.engine.registerHandler("TASK", [](const jobq::Job &) {});
            context.engine.registerHandler("FAIL", [](const jobq::Job &)
                                           { throw std::runtime_error("internal secret path"); });
            context.engine.start();
            context.listening.store(true);
            check(json(jobq::server::listJobsResponse(context, request()))["data"].empty() &&
                      json(jobq::server::queueResponse(context))["data"]["ready"].asUInt64() == 0,
                  "Empty jobs and singleton queue return real zero values");
            auto media = request("", body("TASK", ""));
            media->addHeader("content-type", "text/plain");
            check(jobq::server::submitJobResponse(context, media)->statusCode() == drogon::k415UnsupportedMediaType,
                  "Unsupported media types return 415");
            for (const auto &text : {"", "{", "[]", "null", "{}", "{\"type\":\"TASK\"}",
                                     "{\"type\":\"\",\"payload\":\"\"}", "{\"type\":1,\"payload\":\"\"}",
                                     "{\"type\":\"TASK\",\"payload\":{}}", "{\"type\":\"TASK\",\"payload\":\"\",\"extra\":1}",
                                     "{\"type\":\"TASK\",\"payload\":\"\",\"priority\":\"urgent\"}",
                                     "{\"type\":\"TASK\",\"payload\":\"\",\"retry\":{\"max_retries\":-1}}",
                                     "{\"type\":\"TASK\",\"payload\":\"\",\"retry\":{\"multiplier\":0}}",
                                     "{\"type\":\"TASK\",\"payload\":\"\",\"retry\":{\"unknown\":1}}",
                                     "{\"type\":\"TASK\",\"type\":\"TASK\",\"payload\":\"\"}",
                                     "{\"type\":\"TASK\",\"payload\":\"\",}"})
            {
                  const auto response = jobq::server::submitJobResponse(context, request("", text));
                  check(response->statusCode() == drogon::k400BadRequest && json(response).isMember("error"),
                        "Malformed/missing/unsupported JSON fields and options produce bounded 400 errors");
            }
            check(jobq::server::submitJobResponse(context, request("", body("UNKNOWN", "")))->statusCode() == drogon::k422UnprocessableEntity,
                  "Actual Engine handler validation maps unknown types to 422");
            const std::string payload = "  preserved, C:\\work\n" + std::string(300, 'x');
            const auto accepted = jobq::server::submitJobResponse(context, request("", body("BLOCK", payload)));
            const auto id = json(accepted)["data"]["id"].asString();
            check(accepted->statusCode() == drogon::k201Created && !id.empty() && entered.waitAtLeast(1) && received == payload &&
                      accepted->getHeader("location") == "/api/v1/jobs/" + id,
                  "Real submission returns its actual ID/Location and preserves payload through the real worker");
            const auto active = json(jobq::server::getJobResponse(context, id))["data"];
            check(active["status"] == "RUNNING" && active["attempts_made"].asUInt64() == 1 && active["payload_truncated"] == true &&
                      active["payload_preview"].asString().size() <= 640 && active["payload_bytes"].asUInt64() == payload.size() &&
                      !active.isMember("created_at") && !active.isMember("last_error"),
                  "Inspection uses actual running records and bounded previews without invented fields");
            const auto waiting = jobq::server::submitJobResponse(context, request("", body("TASK", "")));
            const auto pending = json(waiting)["data"]["id"].asString();
            check(json(jobq::server::getJobResponse(context, pending))["data"]["status"] == "PENDING" &&
                      json(jobq::server::queueResponse(context))["data"]["ready"].asUInt64() == 1,
                  "Waiting depth excludes a currently executing job");
            const auto firstPage = json(jobq::server::listJobsResponse(context, request("limit=1")));
            const auto secondPage = json(jobq::server::listJobsResponse(context, request("limit=1&offset=1")));
            check(firstPage["data"].size() == 1 && firstPage["data"][0]["id"] == id &&
                  secondPage["data"][0]["id"] == pending && firstPage["pagination"]["total"].asUInt64() == 2,
                  "Pagination preserves ascending actual IDs and full filtered totals");
            check(json(jobq::server::listJobsResponse(context, request("status=PENDING&type=TASK")))["pagination"]["total"].asUInt64() == 1 &&
                  json(jobq::server::listJobsResponse(context, request("type=NONE")))["data"].empty() &&
                  json(jobq::server::listJobsResponse(context, request("offset=18446744073709551615&limit=100")))["data"].empty(),
                  "Combined filters, empty matches, and far offsets yield bounded collections");
            for (const auto& query : {"limit=0", "limit=101", "limit=-1", "offset=-1", "limit=abc", "offset=1x",
                                     "offset=18446744073709551616", "status=waiting", "type=", "unknown=1", "limit="}) {
                check(jobq::server::listJobsResponse(context, request(query))->statusCode() == drogon::k400BadRequest,
                      "Invalid pagination and unsupported filters return 400");
            }
            for (int index = 0; index < 100; ++index)
            {
                  const auto data = json(jobq::server::queueResponse(context))["data"];
                  if (data["ready"].asUInt64() != 1 || data["delayed"].asUInt64() != 0 || data.isMember("active") || data.isMember("capacity"))
                  {
                        throw std::runtime_error("Queue inspection changed or fabricated state");
                  }
            }
            for (const auto &idText : {"", "0", "-1", "+1", "1x", "18446744073709551616"})
            {
                  check(jobq::server::getJobResponse(context, idText)->statusCode() == drogon::k400BadRequest,
                        "Invalid/overflowing IDs produce 400");
            }
            check(jobq::server::getJobResponse(context, "18446744073709551615")->statusCode() == drogon::k404NotFound,
                  "Unknown valid IDs produce 404 instead of fabricated records");
            context.requestStop();
            check(jobq::server::submitJobResponse(context, request("", body("TASK", "late")))->statusCode() == drogon::k503ServiceUnavailable &&
                      jobq::server::listJobsResponse(context, request())->statusCode() == drogon::k503ServiceUnavailable,
                  "Shutdown closes REST admission before Engine stop begins");
            release.open();
            context.engine.stop();
            check(context.engine.metrics().submittedJobs == 2 && context.engine.metrics().completedExecutions == 2 &&
                      context.engine.getJob(std::stoull(id))->payload == payload,
                  "Inspection/validation do not create extra jobs and all accepted work drains");
      }

      void storageFailure()
      {
            jobq_test::TemporaryDirectory directory("rest_storage");
            jobq::server::ServerConfig config;
            config.storagePath = directory.journal();
            jobq::server::ServerContext context(config);
            context.engine.registerHandler("FAIL", [](const jobq::Job &)
                                           { throw std::runtime_error("private execution error"); });
            context.engine.start();
            context.listening.store(true);
            const auto response = jobq::server::submitJobResponse(context, request("", body("FAIL", "")));
            const auto id = json(response)["data"]["id"].asString();
            context.engine.stop();
            context.listening.store(false);
            check(context.engine.getJob(std::stoull(id))->status == jobq::JobStatus::Failed &&
                      response->body().find("private execution error") == std::string_view::npos,
                  "Handler failures remain real job state rather than leaked exception details");
            {
                jobq::server::ServerContext recovered(config);
                recovered.engine.start();
                recovered.listening.store(true);
                check(json(jobq::server::getJobResponse(recovered, id))["data"]["status"] == "FAILED" &&
                      json(jobq::server::listJobsResponse(recovered, request()))["pagination"]["total"].asUInt64() == 1,
                      "REST lookup and listing use actual recovered records");
                recovered.engine.stop();
            }
            jobq_test::TemporaryDirectory broken("rest_storage_failure");
            config.storagePath = broken.journal();
            jobq::server::ServerContext failing(config);
            failing.engine.registerHandler("TASK", [](const jobq::Job&) {});
            failing.engine.start();
            failing.listening.store(true);
            std::filesystem::create_directory(broken.journal());
            const auto failure = jobq::server::submitJobResponse(failing, request("", body("TASK", "")));
            check(failure->statusCode() == drogon::k500InternalServerError &&
                  json(failure)["error"]["code"] == "INTERNAL_ERROR" && failing.engine.listJobs().empty() &&
                  failure->body().find(broken.journal().string()) == std::string_view::npos,
                  "Storage failure rolls back submission and returns a generic error without paths");
            failing.engine.stop();
      }

}

int main()
{
      try
      {
            validationAndJobs();
            storageFailure();
      }
      catch (const std::exception &error)
      {
            check(false, error.what());
      }
      return jobq_test::testsFailed() ? 1 : 0;
}
