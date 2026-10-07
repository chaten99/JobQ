#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "executor/JobExecutor.h"
#include "queue/JobQueue.h"
#include "worker/Worker.h"

namespace jobq {

class WorkerPool {
public:
    WorkerPool(
        JobQueue& queue,
        JobExecutor& executor,
        std::size_t workerCount
    );

    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start();

    void stop();

    std::size_t size() const;

private:
    JobQueue& queue_;
    JobExecutor& executor_;

    std::vector<std::unique_ptr<Worker>> workers_;

    bool started_{false};
    bool stopped_{false};
};

} // namespace jobq