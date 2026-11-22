#include "ServerConfig.h"
#include "ServerRuntime.h"
#include "HealthRoutes.h"
#include "InspectionRoutes.h"

#include <future>
#include <sstream>
#include <stdexcept>

#include "helpers/TestSupport.h"

int main()
{
      using jobq_test::check;
      try
      {
            const auto defaults = jobq::server::parseConfig({});
            check(defaults.address == "127.0.0.1" && defaults.port == 8080 && defaults.httpThreads == 2 &&
                      defaults.engineWorkers == 4 && defaults.maxBodyBytes == 1048576 && defaults.maxConnections == 1024,
                  "Default listener is bounded and local with separate HTTP/Engine thread counts");
            const auto custom = jobq::server::parseConfig({"--bind", "::1", "--port", "0", "--http-threads", "1",
                                                           "--engine-workers", "2", "--max-body-bytes", "4096", "--max-connections", "32"});
            check(custom.port == 0 && custom.address == "::1" && custom.httpThreads == 1 && custom.engineWorkers == 2 &&
                      custom.maxBodyBytes == 4096 && custom.maxConnections == 32,
                  "Custom numeric IPv6 and ephemeral port parse");
            for (const auto &arguments : {std::vector<std::string>{"--port", "65536"}, {"--port", "-1"}, {"--port", "1x"}, {"--port", "18446744073709551616"}, {"--port"}, {"--bind", "localhost"}, {"--http-threads", "0"}, {"--http-threads", "65"}, {"--engine-workers", "0"}, {"--max-body-bytes", "0"}, {"--max-connections", "0"}, {"--unknown", "1"}, {"--port", "1", "--port", "2"}, {"--help", "--port", "1"}})
            {
                  check(jobq_test::throwsWith<std::invalid_argument>([&]
                                                                     { jobq::server::parseConfig(arguments); }, ""),
                        "Invalid/missing/repeated configuration is rejected");
            }
            jobq_test::Counter entered;
            jobq_test::Gate release;
            jobq::server::ServerContext context(defaults);
            const auto request = drogon::HttpRequest::newHttpRequest();
            const auto unavailable = [&] {
                for (const auto inspect : {jobq::server::workersResponse, jobq::server::runtimeResponse}) {
                    const auto response = inspect(context, request);
                    if (response->statusCode() != drogon::k503ServiceUnavailable ||
                        (!response->getJsonObject() || (*response->getJsonObject())["error"]["code"] != "ENGINE_UNAVAILABLE")) return false;
                }
                return true;
            };
            check(unavailable(), "Inspection is unavailable before startup");
            check(!context.ready() && jobq::server::readinessResponse(context)->statusCode() == drogon::k503ServiceUnavailable,
                  "Readiness returns 503 before Engine and listener startup");
            context.engine.registerHandler("BLOCK", [&](const jobq::Job&) {
                entered.add();
                if (!release.wait()) throw std::runtime_error("Release timed out");
            });
            context.engine.start();
            check(!context.ready() && unavailable(), "A running Engine alone does not imply HTTP readiness");
            context.markListening();
        check(context.ready(),
                  "Readiness requires the started listener and running Engine");
            context.submit("BLOCK", "");
            check(entered.waitAtLeast(1), "A real worker is executing before shutdown");
            context.requestStop();
            check(unavailable(), "Inspection is unavailable after shutdown is requested");
            check(!context.ready() && jobq::server::readinessResponse(context)->statusCode() == drogon::k503ServiceUnavailable,
                  "Shutdown immediately returns readiness 503");
            auto draining = std::async(std::launch::async, [&] { context.engine.stop(); });
            check(jobq_test::waitUntil([&] { return context.engine.snapshot().state == jobq::EngineState::Stopping; }),
                  "The Engine enters stopping while an accepted job is held");
            check(unavailable(), "Inspection returns generic unavailable responses while workers drain");
            release.open();
            draining.get();
            check(unavailable(), "Inspection stays unavailable after workers stop");
            check(context.engine.snapshot().workersStopped, "Context Engine workers are joined");
            jobq::Engine *acquired = nullptr;
            jobq::server::ServerRuntime failing(custom, [&](jobq::Engine &engine)
                                                {
            acquired = &engine;
            engine.start();
            throw std::runtime_error("initializer failure"); });
            std::ostringstream output, errors;
            check(failing.run(output, errors) == 1 && acquired && acquired->snapshot().workersStopped &&
                      errors.str().find("initializer failure") != std::string::npos && !failing.ready(),
                  "Partial Engine initialization failure is reported and workers cleaned up");
            check(failing.run(output, errors) == 1, "Server runtime cannot restart");
      }
      catch (const std::exception &error)
      {
            check(false, error.what());
      }
      return jobq_test::testsFailed() ? 1 : 0;
}
