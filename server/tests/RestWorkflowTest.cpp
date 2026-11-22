#include "ServerRuntime.h"
#include "jobq/Version.h"

#include <drogon/drogon.h>
#include <drogon/WebSocketClient.h>
#include <trantor/net/EventLoopThread.h>
#include <future>
#include <sstream>
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

struct EventClient {
    struct Inbox {
        std::mutex mutex;
        std::vector<Json::Value> events;
        std::atomic<bool> closed{false};
    };

    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    drogon::WebSocketClientPtr socket;

    EventClient(const std::string& address, trantor::EventLoop* loop)
        : socket(drogon::WebSocketClient::newWebSocketClient("ws://" + address.substr(7), loop)) {
        socket->setMessageHandler([inbox = inbox](std::string&& message, const auto&, auto type) {
            if (type != drogon::WebSocketMessageType::Text) return;
            std::istringstream input(message);
            Json::Value event;
            std::string errors;
            if (!Json::parseFromStream(Json::CharReaderBuilder{}, input, &event, &errors)) return;
            std::lock_guard lock(inbox->mutex);
            inbox->events.push_back(std::move(event));
        });
        socket->setConnectionClosedHandler([inbox = inbox](const auto& client) {
            inbox->closed.store(true);
            client->stop();
        });
    }

    bool connect() {
        auto promise = std::make_shared<std::promise<bool>>();
        auto ready = promise->get_future();
        auto reported = std::make_shared<std::atomic<bool>>(false);
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/api/v1/events");
        socket->connectToServer(request, [promise, reported](drogon::ReqResult result, const auto& response, const auto&) {
            if (reported->exchange(true)) return;
            promise->set_value(result == drogon::ReqResult::Ok && response && response->statusCode() == drogon::k101SwitchingProtocols);
        });
        return ready.wait_for(3s) == std::future_status::ready && ready.get();
    }

    std::vector<Json::Value> events() const {
        std::lock_guard lock(inbox->mutex);
        return inbox->events;
    }

    ~EventClient() { socket->stop(); }
};

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
            const auto address = "http://127.0.0.1:" + std::to_string(bound.get());
            auto http = drogon::HttpClient::newHttpClient(address, loop.getLoop());
            auto secondClient = drogon::HttpClient::newHttpClient(address, loop.getLoop());
            auto send = [&](drogon::HttpMethod method, const std::string& path, const std::string& body = "",
                            const std::string& contentType = "application/json", const drogon::HttpClientPtr& connection = {}) {
                auto request = drogon::HttpRequest::newHttpRequest();
                request->setMethod(method);
                request->setPath(path);
                request->setPathEncode(false);
                if (!body.empty()) request->setBody(body);
                request->setCustomContentTypeString(contentType);
                const auto [result, response] = (connection ? connection : http)->sendRequest(request, 3.0);
                if (result != drogon::ReqResult::Ok || !response) throw std::runtime_error("HTTP request failed");
                return response;
            };
            check(jobq_test::waitUntil([&] { return send(drogon::Get, "/health/ready")->statusCode() == drogon::k200OK; }),
                  "The HTTP workflow waits for real listener and Engine readiness");
            auto inspectRuntime = [&](const drogon::HttpClientPtr& connection = {}) {
                const auto response = send(drogon::Get, "/api/v1/runtime", "", "application/json", connection);
                const auto body = parsed(response);
                const auto& data = body["data"];
                if (response->statusCode() != drogon::k200OK ||
                    response->getHeader("cache-control") != "no-store" ||
                    response->getHeader("content-type").find("application/json") == std::string::npos ||
                    data["server"]["state"] != "running" || data["engine"]["state"] != "running" ||
                    data["engine"]["accepting_jobs"] != true || data["engine"]["configured_workers"].asUInt64() != 2 ||
                    data["version"]["jobq"] != std::string(jobq::version::string) ||
                    !data["server"]["uptime_seconds"].isUInt64() || data["server"].size() != 2) {
                    throw std::runtime_error("Unexpected runtime snapshot");
                }
                return data["server"]["uptime_seconds"].asUInt64();
            };
            auto inspectWorkers = [&](const drogon::HttpClientPtr& connection = {}) {
                const auto response = send(drogon::Get, "/api/v1/workers", "", "application/json", connection);
                const auto data = parsed(response)["data"];
                return response->statusCode() == drogon::k200OK && data["state"] == "running" &&
                       data["configured_workers"].asUInt64() == 2 && data.size() == 2;
            };
            const auto initialUptime = inspectRuntime();
            check(inspectWorkers(), "Workers expose actual configured pool state");
            for (const auto* path : {"/api/v1/workers", "/api/v1/runtime"}) {
                const auto invalid = send(drogon::Get, std::string(path) + "?limit=1");
                check(invalid->statusCode() == drogon::k400BadRequest &&
                      parsed(invalid)["error"]["code"] == "INVALID_REQUEST", "Inspection rejects query arguments");
                check(send(drogon::Post, path)->statusCode() == drogon::k405MethodNotAllowed, "Inspection is read-only");
            }
            EventClient eventOne(address, loop.getLoop());
            EventClient eventTwo(address, loop.getLoop());
            if (!eventOne.connect() || !eventTwo.connect()) throw std::runtime_error("WebSocket upgrades failed");
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
            auto runtimeReader = std::async(std::launch::async, [&] {
                return inspectRuntime(secondClient) >= initialUptime;
            });
            const auto first = submit("BLOCK", "first");
            const auto second = submit("BLOCK", "second");
            check(active.waitAtLeast(2), "Two real workers execute HTTP-submitted jobs");
            const auto pending = submit("ECHO", "  payload, C:\\work  ");
            check(parsed(send(drogon::Get, "/api/v1/jobs/" + first))["data"]["status"] == "RUNNING" &&
                  parsed(send(drogon::Get, "/api/v1/jobs/" + pending))["data"]["status"] == "PENDING" &&
                  parsed(send(drogon::Get, "/api/v1/queue"))["data"]["ready"].asUInt64() == 1,
                  "HTTP records distinguish executing jobs from current ready depth");
            check(runtimeReader.get(), "Runtime inspection stays valid during job submissions");
            const auto page = parsed(send(drogon::Get, "/api/v1/jobs?limit=1&offset=1&status=RUNNING&type=BLOCK"));
            check(page["pagination"]["total"].asUInt64() == 2 && page["data"].size() == 1 && page["data"][0]["id"] == second,
                  "Actual URL filters/pagination preserve deterministic ordering and filtered total");
            for (int index = 0; index < 20; ++index) {
                if (parsed(send(drogon::Get, "/api/v1/queue"))["data"]["ready"].asUInt64() != 1) {
                    throw std::runtime_error("Concurrent queue inspection changed waiting depth");
                }
            }
            auto inspect = [&](const drogon::HttpClientPtr& connection) {
                for (int index = 0; index < 10; ++index) {
                    if (!inspectWorkers(connection) || inspectRuntime(connection) < initialUptime) return false;
                }
                return true;
            };
            auto readerOne = std::async(std::launch::async, inspect, http);
            auto readerTwo = std::async(std::launch::async, inspect, secondClient);
            check(readerOne.get() && readerTwo.get(), "Concurrent HTTP readers preserve a real queue snapshot while workers execute");
            check(jobq_test::waitUntil([&] { return eventOne.events().size() >= 2 && eventTwo.events().size() >= 2; }),
                  "Both WebSocket clients receive real running transitions");
            check(eventOne.events() == eventTwo.events(), "Both subscribers receive identical sequence and data");
            eventTwo.socket->getConnection()->send("{invalid");
            check(jobq_test::waitUntil([&] { return eventTwo.inbox->closed.load(); }),
                  "Unsupported client messages close only that subscription");
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
            check(jobq_test::waitUntil([&] { return eventOne.events().size() >= 13; }),
                  "The remaining WebSocket client receives completion, failure and retry transitions");
            const auto updates = eventOne.events();
            std::uint64_t sequence = 0;
            std::vector<std::string> retryStates, rapidStates, failureStates;
            for (const auto& update : updates) {
                const auto& data = update["data"];
                check(update["event"] == "job.status_changed" && update["sequence"] == std::to_string(++sequence) &&
                      data["job_id"].isString() && data.isMember("previous_status") && !data.isMember("payload"),
                      "Published events have the versioned envelope and monotonic string sequence");
                auto describe = [&] {
                    return data["previous_status"].asString() + ":" + data["status"].asString() +
                           ":" + std::to_string(data["attempts_made"].asUInt64());
                };
                if (data["job_id"] == pending) rapidStates.push_back(describe());
                if (data["job_id"] == failed) failureStates.push_back(describe());
                if (data["job_id"] == retried) retryStates.push_back(describe());
            }
            check(rapidStates == std::vector<std::string>{"PENDING:RUNNING:1", "RUNNING:COMPLETED:1"} &&
                  failureStates == std::vector<std::string>{"PENDING:RUNNING:1", "RUNNING:FAILED:1"} &&
                  retryStates == std::vector<std::string>{"PENDING:RUNNING:1", "RUNNING:FAILED:1",
                      "FAILED:PENDING:1", "PENDING:RUNNING:2", "RUNNING:COMPLETED:2"},
                  "Rapid success, failure and retry preserve exact per-job transition order and attempts");
            check(inspectWorkers() && inspectRuntime() > initialUptime, "Uptime advances through the delayed retry workflow");
            const auto final = submit("FINAL", "drain on shutdown");
            check(finalEntered.waitAtLeast(1), "An accepted final job is active before shutdown");
            runtime.requestStop();
            check(jobq_test::waitUntil([&] { return engine->snapshot().state == jobq::EngineState::Stopping; }),
                  "Server shutdown stops HTTP processing before waiting for accepted Engine work");
            check(jobq_test::waitUntil([&] { return eventOne.inbox->closed.load(); }),
                  "Shutdown closes the remaining WebSocket subscription while work drains");
            EventClient late(address, loop.getLoop());
            check(!late.connect() || jobq_test::waitUntil([&] { return late.inbox->closed.load(); }),
                  "Shutdown rejects or closes new WebSocket connections");
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
