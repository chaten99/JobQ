#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace jobq {

struct ExecutionSample {
    using Clock = std::chrono::steady_clock;

    std::uint64_t jobId{0};
    std::size_t workerId{0};
    std::size_t attempt{0};

    Clock::duration queueWait{};
    Clock::time_point finishedAt{};
    Clock::duration executionDuration{};

    bool succeeded{false};
};

class ExecutionObserver {
public:
    virtual ~ExecutionObserver() = default;

    virtual void onExecutionFinished(
        const ExecutionSample& sample
    ) noexcept = 0;
};

}
