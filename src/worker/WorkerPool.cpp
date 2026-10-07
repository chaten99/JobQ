#include "worker/WorkerPool.h"

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
                    executor_
            )
        );
        }
    }

WorkerPool::~WorkerPool() {
        stop();
    }

void WorkerPool::start() {
    if (started_) {
            throw std::logic_error(
            "Worker pool has already been started"
        );
        }

    if (stopped_) {
            throw std::logic_error(
            "Worker pool cannot be restarted"
        );
        }

    try {
        for (auto& worker : workers_) {
                worker->start();
            }

            started_ = true;
        }
    catch (...) {
            queue_.close();

        for (auto& worker : workers_) {
                worker->join();
            }

            throw;
        }
    }

void WorkerPool::stop() {
    if (!started_ || stopped_) {
            return;
        }

        queue_.close();

    for (auto& worker : workers_) {
            worker->join();
        }

        stopped_ = true;
    }

std::size_t WorkerPool::size() const {
        return workers_.size();
    }

} // namespace jobq