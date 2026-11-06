#include "worker/WorkerPool.h"

#include <exception>
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
    try {
        stop();
    } catch (...) {
    }
}

void WorkerPool::start() {
    std::exception_ptr failure;

    {
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
            return;
        } catch (...) {
            failure = std::current_exception();
            stopping_ = true;
        }
    }

    queue_.close();
    joinAllWorkers();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        stopping_ = false;
    }

    stoppedCv_.notify_all();
    std::rethrow_exception(failure);
}

void WorkerPool::stop() {
    for (const auto& worker : workers_) {
        if (worker->isCurrentThread()) {
            throw std::logic_error(
                "Worker pool cannot be stopped from one of its own workers"
            );
        }
    }

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
    std::exception_ptr joinFailure = joinAllWorkers();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        stopping_ = false;
    }

    stoppedCv_.notify_all();

    if (joinFailure) {
        std::rethrow_exception(joinFailure);
    }
}

std::exception_ptr WorkerPool::joinAllWorkers() noexcept {
    std::exception_ptr first;

    for (auto& worker : workers_) {
        try {
            worker->join();
        } catch (...) {
            if (!first) {
                first = std::current_exception();
            }
        }
    }

    return first;
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
