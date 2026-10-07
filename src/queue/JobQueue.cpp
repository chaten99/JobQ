#include "queue/JobQueue.h"

namespace jobq {

bool JobQueue::push(std::shared_ptr<Job> job) {
    if (!job) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            return false;
        }

        jobs_.push(std::move(job));
    }

    condition_.notify_one();
    return true;
}

std::shared_ptr<Job> JobQueue::tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (jobs_.empty()) {
        return nullptr;
    }

    auto job = std::move(jobs_.front());
    jobs_.pop();

    return job;
}

std::shared_ptr<Job> JobQueue::waitAndPop() {
    std::unique_lock<std::mutex> lock(mutex_);

    condition_.wait(lock, [this] {
        return closed_ || !jobs_.empty();
    });

    if (jobs_.empty()) {
        return nullptr;
    }

    auto job = std::move(jobs_.front());
    jobs_.pop();

    return job;
}

void JobQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    condition_.notify_all();
}

bool JobQueue::isClosed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

bool JobQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty();
}

std::size_t JobQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

} // namespace jobq