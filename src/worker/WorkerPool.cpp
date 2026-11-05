#include "worker/WorkerPool.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace jobq {

WorkerPool::WorkerPool(
    JobQueue& queue,
    JobExecutor& executor,
    std::size_t workerCount
)
    : queue_(queue),
      executor_(executor) {
    initializeWorkers(workerCount);
}

WorkerPool::WorkerPool(
    JobQueue& queue,
    JobExecutor& executor,
    std::size_t workerCount,
    JobMetrics& metrics
)
    : queue_(queue),
      executor_(executor),
      metrics_(&metrics) {
    initializeWorkers(workerCount);
}

void WorkerPool::initializeWorkers(std::size_t workerCount) {
    if (workerCount == 0) {
        throw std::invalid_argument(
            "Worker count must be greater than zero"
        );
    }

    workers_.reserve(workerCount);

    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.push_back(
            std::make_unique<Worker>(
                i + 1,
                queue_,
                executor_,
                metrics_
            )
        );
    }
}

WorkerPool::~WorkerPool() {
    stop();
}

void WorkerPool::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (stopping_ || stopped_) {
        throw std::logic_error(
            "Worker pool cannot be restarted"
        );
    }

    if (started_) {
        throw std::logic_error(
            "Worker pool has already been started"
        );
    }

    try {
        for (auto& worker : workers_) {
            worker->start();
        }

        started_ = true;
    } catch (...) {
        stopped_ = true;
        queue_.close();

        for (auto& worker : workers_) {
            worker->join();
        }

        stoppedCv_.notify_all();
        throw;
    }
}

void WorkerPool::stop() {
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (stopped_) {
            return;
        }

        if (stopping_) {
            stoppedCv_.wait(lock, [this] { return stopped_; });
            return;
        }

        stopping_ = true;
    }

    queue_.close();

    if (started_) {
        for (auto& worker : workers_) {
            worker->join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        stopping_ = false;
    }

    stoppedCv_.notify_all();
}

std::size_t WorkerPool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

bool WorkerPool::isStarted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_;
}

bool WorkerPool::isStopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

} // namespace jobq
