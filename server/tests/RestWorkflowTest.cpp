#include "ServerRuntime.h"

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>
#include <future>
#include <thread>

#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;

Json::Value parsed(const drogon::HttpResponsePtr& response) {
    const auto value = response->getJsonObject();
    if (!value) throw std::runtime_error("HTTP response was not JSON");
    return *value;
}

}

int main() {
    jobq_test::Counter active;
    jobq_test::Gate release;
    jobq_test::Gate finalRelease;
    jobq_test::Counter finalEntered;
    jobq::Engine* engine = nullptr;
    jobq::server::ServerConfig config;
    config.port = 0;
    config.engineWorkers = 2;
    jobq::server::ServerRuntime runtime(config, [&](jobq::Engine& current) {
        engine = &current;
        current.registerHandler("BLOCK", [&](const jobq::Job&) {
            active.add();
            if (!release.wait()) throw std::runtime_error("Release timed out");
        });
        current.registerHandler("ECHO", [](const jobq::Job&) {});
        current.registerHandler("FAIL", [](const jobq::Job&) { throw std::runtime_error("private failure detail"); });
        current.registerHandler("RETRY", [](const jobq::Job& job) {
            if (job.getAttemptsMade() == 1) throw std::runtime_error("retry once");
        });
        current.registerHandler("FINAL", [&](const jobq::Job&) {
            finalEntered.add();
            if (!finalRelease.wait()) throw std::runtime_error("Final release timed out");
        });
    });
    std::promise<unsigned> port;
    auto bound = port.get_future();
    drogon::app().registerBeginningAdvice([&] { port.set_value(drogon::app().getListeners().front().toPort()); });
    std::exception_ptr clientFailure;
    std::thread client([&] {
        try {
            if (bound.wait_for(5s) != std::future_status::ready) throw std::runtime_error("Listener never initialized");
            trantor::EventLoopThread loop;
            loop.run();
            auto http = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(bound.get()), loop.getLoop());
            auto send = [&](drogon::HttpMethod method, const std::string& path, const std::string& body = "",
                            const std::string& contentType = "application/json") {
                auto request = drogon::HttpRequest::newHttpRequest();
                request->setMethod(method);
                request->setPath(path);
                request->setPathEncode(false);
                if (!body.empty()) request->setBody(body);
                request->setCustomContentTypeString(contentType);
                const auto [result, response] = http->sendRequest(request, 3.0);
                if (result != drogon::ReqResult::Ok || !response) throw std::runtime_error("HTTP request failed");
                return response;
            };
            check(jobq_test::waitUntil([&] { return send(drogon::Get, "/health/ready")->statusCode() == drogon::k200OK; }),
                  "The HTTP workflow waits for real listener and Engine readiness");
            const auto empty = parsed(send(drogon::Get, "/api/v1/jobs"));
            check(empty["data"].empty() && empty["pagination"]["limit"].asUInt64() == 50,
                  "Real HTTP empty collection uses default pagination");
            auto submit = [&](const std::string& type, const std::string& payload, bool retry = false) {
                Json::Value value;
                value["type"] = type;
                value["payload"] = payload;
                if (retry) {
                    value["priority"] = "HIGH";
                    value["retry"]["max_retries"] = 1;
                    value["retry"]["initial_delay_ms"] = 1000;
                    value["retry"]["max_delay_ms"] = 1000;
                    value["retry"]["multiplier"] = 2.0;
                }
                const auto response = send(drogon::Post, "/api/v1/jobs", Json::writeString(Json::StreamWriterBuilder{}, value));
                if (response->statusCode() != drogon::k201Created) throw std::runtime_error("Expected actual HTTP submission: " + std::string(response->body()));
                return parsed(response)["data"]["id"].asString();
            };
            const auto first = submit("BLOCK", "first");
            const auto second = submit("BLOCK", "second");
            check(active.waitAtLeast(2), "Two real workers execute HTTP-submitted jobs");
            const auto pending = submit("ECHO", "  payload, C:\\work  ");
            check(parsed(send(drogon::Get, "/api/v1/jobs/" + first))["data"]["status"] == "RUNNING" &&
                  parsed(send(drogon::Get, "/api/v1/jobs/" + pending))["data"]["status"] == "PENDING" &&
                  parsed(send(drogon::Get, "/api/v1/queue"))["data"]["ready"].asUInt64() == 1,
                  "HTTP records distinguish executing jobs from current ready depth");
            const auto page = parsed(send(drogon::Get, "/api/v1/jobs?limit=1&offset=1&status=RUNNING&type=BLOCK"));
            check(page["pagination"]["total"].asUInt64() == 2 && page["data"].size() == 1 && page["data"][0]["id"] == second,
                  "Actual URL filters/pagination preserve deterministic ordering and filtered total");
            for (int index = 0; index < 20; ++index) {
                if (parsed(send(drogon::Get, "/api/v1/queue"))["data"]["ready"].asUInt64() != 1) {
                    throw std::runtime_error("Concurrent queue inspection changed waiting depth");
                }
            }
            auto inspect = [&] {
                for (int index = 0; index < 10; ++index) {
                    if (parsed(send(drogon::Get, "/api/v1/queue"))["data"]["ready"].asUInt64() != 1) return false;
                }
                return true;
            };
            auto readerOne = std::async(std::launch::async, inspect);
            auto readerTwo = std::async(std::launch::async, inspect);
            check(readerOne.get() && readerTwo.get(), "Concurrent HTTP readers preserve a real queue snapshot while workers execute");
            for (const auto& query : {"limit=0", "offset=-1", "limit=101", "limit=1&limit=2", "offset=18446744073709551616",
                                     "status=invalid", "type=", "extra=1", "limit=1&"}) {
                const auto response = send(drogon::Get, "/api/v1/jobs?" + std::string(query));
                check(response->statusCode() == drogon::k400BadRequest && parsed(response).isMember("error"),
                      "Invalid real HTTP query parameters use JSON 400 responses");
            }
            check(send(drogon::Post, "/api/v1/jobs", "{}", "text/plain")->statusCode() == drogon::k415UnsupportedMediaType &&
                  send(drogon::Post, "/api/v1/jobs", "{broken")->statusCode() == drogon::k400BadRequest &&
                  send(drogon::Post, "/api/v1/jobs", "{\"type\":\"UNKNOWN\",\"payload\":\"\"}")->statusCode() == drogon::k422UnprocessableEntity &&
                  send(drogon::Post, "/api/v1/queue", "{}")->statusCode() == drogon::k405MethodNotAllowed &&
                  send(drogon::Get, "/api/v1/jobs/0")->statusCode() == drogon::k400BadRequest &&
                  send(drogon::Get, "/api/v1/jobs/18446744073709551615")->statusCode() == drogon::k404NotFound,
                  "Real HTTP content/JSON/type/method/ID failures preserve their categories");
            release.open();
            check(jobq_test::waitUntil([&] { return engine->metrics().completedExecutions == 3; }),
                  "All accepted blocked and waiting HTTP work is processed");
            check(engine->getJob(std::stoull(pending))->payload == "  payload, C:\\work  ",
                  "Real HTTP submission preserves original payload bytes");
            const auto failed = submit("FAIL", "");
            const auto retried = submit("RETRY", "retry payload", true);
            check(jobq_test::waitUntil([&] { return engine->snapshot().delayedJobs == 1; }),
                  "The actual retry policy places a failed attempt into the delayed queue");
            const auto delayed = parsed(send(drogon::Get, "/api/v1/jobs/" + retried))["data"];
            check(delayed["status"] == "PENDING" && delayed["attempts_made"].asUInt64() == 1 &&
                  parsed(send(drogon::Get, "/api/v1/queue"))["data"]["delayed"].asUInt64() == 1,
                  "HTTP retry records and delayed queue entries are genuine snapshots");
            check(jobq_test::waitUntil([&] { return engine->getJob(std::stoull(retried))->status == jobq::JobStatus::Completed; }),
                  "The existing worker retry completes without an HTTP-side scheduler");
            check(parsed(send(drogon::Get, "/api/v1/jobs/" + failed))["data"]["status"] == "FAILED" &&
                  parsed(send(drogon::Get, "/api/v1/jobs/" + retried))["data"]["attempts_made"].asUInt64() == 2,
                  "Final API records expose actual failure and retry-success attempts");
            const auto final = submit("FINAL", "drain on shutdown");
            check(finalEntered.waitAtLeast(1), "An accepted final job is active before shutdown");
            runtime.requestStop();
            check(jobq_test::waitUntil([&] { return engine->snapshot().state == jobq::EngineState::Stopping; }),
                  "Server shutdown stops HTTP processing before waiting for accepted Engine work");
            finalRelease.open();
            check(engine->getJob(std::stoull(final)).has_value(), "The shared context remains alive throughout shutdown");
        } catch (...) {
            clientFailure = std::current_exception();
            release.open();
            finalRelease.open();
            runtime.requestStop();
        }
    });
    std::ostringstream output, errors;
    const auto result = runtime.run(output, errors);
    client.join();
    if (clientFailure) {
        try { std::rethrow_exception(clientFailure); }
        catch (const std::exception& error) { check(false, error.what()); }
    }
    check(result == 0 && errors.str().empty() && engine && engine->snapshot().workersStopped &&
          engine->metrics().submittedJobs == 6 && engine->metrics().completedExecutions == 5 &&
          engine->metrics().failedExecutions == 2 && engine->metrics().scheduledRetries == 1,
          "HTTP workflow joins workers and preserves six jobs versus seven execution attempts");
    return jobq_test::testsFailed() ? 1 : 0;
}
