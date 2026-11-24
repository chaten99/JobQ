#include "ServerRuntime.h"

#include <drogon/drogon.h>
#include <sstream>

int main()
{
    jobq::server::ServerConfig config;
    config.port = 0;
    config.engineWorkers = 2;
    jobq::Engine *engine = nullptr;
    jobq::server::ServerRuntime runtime(config, [&](jobq::Engine &current)
                                        { engine = &current; });
    drogon::app().registerBeginningAdvice([&]
                                          { drogon::app().getLoop()->runAfter(0.1, [&]
                                                                              { runtime.requestStop(); runtime.requestStop(); }); });
    std::ostringstream output, errors;
    const auto result = runtime.run(output, errors);
    return result == 0 && errors.str().empty() && engine && engine->snapshot().workersStopped &&
                   !runtime.ready() && output.str().find("Engine workers joined.") != std::string::npos
               ? 0
               : 1;
}
