#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>

#include "cli/CommandDispatcher.h"
#include "jobq/Engine.h"

namespace jobq::cli {

enum class RuntimeState { Created, Starting, Running, Stopping, Stopped, Failed };

struct RuntimeSnapshot {
    RuntimeState state{RuntimeState::Created};
    std::optional<EngineSnapshot> engine;
    std::optional<std::chrono::nanoseconds> uptime;
};

class InteractiveRuntime {
public:
    using EngineInitializer = std::function<void(Engine&)>;

    explicit InteractiveRuntime(EngineConfig config = {}, EngineInitializer initializeEngine = {});

    InteractiveRuntime(const InteractiveRuntime&) = delete;
    InteractiveRuntime& operator=(const InteractiveRuntime&) = delete;
    InteractiveRuntime(InteractiveRuntime&&) = delete;
    InteractiveRuntime& operator=(InteractiveRuntime&&) = delete;

    int run(std::istream& input, std::ostream& output, std::ostream& errorOutput);
    CommandDispatcher& commands() noexcept;
    Engine& engine();
    RuntimeSnapshot snapshot() const;

private:
    void setState(RuntimeState state);

    EngineConfig config_;
    EngineInitializer initializeEngine_;
    CommandDispatcher commands_;
    bool used_{false};
    mutable std::mutex mutex_;
    RuntimeState state_{RuntimeState::Created};
    std::optional<std::chrono::steady_clock::time_point> startedAt_;
    std::optional<std::chrono::steady_clock::time_point> stoppedAt_;
    std::unique_ptr<Engine> engine_;
};

}
