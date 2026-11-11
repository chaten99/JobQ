#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

#include "executor/JobExecutor.h"
#include "queue/JobQueue.h"
#include "worker/Worker.h"

namespace jobq {

class ExecutionObserver;
class JobMetrics;

class WorkerPool {
public:
    WorkerPool(
        JobQueue& queue,
        JobExecutor& executor,
        std::size_t workerCount
    );

    WorkerPool(
        JobQueue& queue,
        JobExecutor& executor,
        std::size_t workerCount,
        JobMetrics& metrics
    );

    WorkerPool(
        JobQueue& queue,
        JobExecutor& executor,
        std::size_t workerCount,
        JobMetrics& metrics,
        ExecutionObserver& observer
    );

    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start();

    void stop();

    std::size_t size() const;

    bool isStarted() const;

    bool isStopped() const;

private:
    void initializeWorkers(std::size_t workerCount);

    std::exception_ptr joinAllWorkers() noexcept;

    JobQueue& queue_;
    JobExecutor& executor_;
    JobMetrics* metrics_{nullptr};
    ExecutionObserver* observer_{nullptr};

    std::vector<std::unique_ptr<Worker>> workers_;

    mutable std::mutex mutex_;
    mutable std::condition_variable stoppedCv_;
    bool started_{false};
    bool stopping_{false};
    bool stopped_{false};
};

} // namespace jobq
