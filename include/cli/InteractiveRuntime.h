#pragma once

#include <iosfwd>
#include <memory>

#include "cli/CommandDispatcher.h"
#include "jobq/Engine.h"

namespace jobq::cli {

class InteractiveRuntime {
public:
    explicit InteractiveRuntime(EngineConfig config = {});

    InteractiveRuntime(const InteractiveRuntime&) = delete;
    InteractiveRuntime& operator=(const InteractiveRuntime&) = delete;
    InteractiveRuntime(InteractiveRuntime&&) = delete;
    InteractiveRuntime& operator=(InteractiveRuntime&&) = delete;

    int run(std::istream& input, std::ostream& output, std::ostream& errorOutput);
    CommandDispatcher& commands() noexcept;
    Engine& engine();

private:
    EngineConfig config_;
    CommandDispatcher commands_;
    std::unique_ptr<Engine> engine_;
    bool used_{false};
};

}
