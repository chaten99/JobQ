#pragma once

#include <thread>

#include "executor/JobExecutor.h"
#include "queue/JobQueue.h"

namespace jobq {

class Worker {
public:
    Worker(
        JobQueue& queue,
        JobExecutor& executor
    );

    ~Worker() = default;

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();

    void join();

private:
    void run();

    JobQueue& queue_;
    JobExecutor& executor_;
    std::thread thread_;
};

} // namespace jobq