#pragma once

#include <cstddef>
#include <thread>

#include "executor/JobExecutor.h"
#include "queue/JobQueue.h"

namespace jobq {

class JobMetrics;

class Worker {
public:
    Worker(
        std::size_t id,
        JobQueue& queue,
        JobExecutor& executor,
        JobMetrics* metrics = nullptr
    );

    ~Worker() = default;

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();

    void join();

private:
    void run();

    std::size_t id_;

    JobQueue& queue_;
    JobExecutor& executor_;
    JobMetrics* metrics_{nullptr};

    std::thread thread_;
};

} // namespace jobq