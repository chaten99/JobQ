#pragma once

#include <cstddef>

#include "metrics/JobMetrics.h"

namespace jobq {

enum class EngineState { Created, Starting, Running, Stopping, Stopped };

struct EngineSnapshot {
    EngineState state{EngineState::Created};
    std::size_t workerCount{0};
    bool workersStarted{false};
    bool workersStopping{false};
    bool workersStopped{false};
    std::size_t readyJobs{0};
    std::size_t delayedJobs{0};
    bool queueClosed{false};
    JobMetricsSnapshot metrics;
};

}
