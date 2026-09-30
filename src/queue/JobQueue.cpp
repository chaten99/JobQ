#include "queue/JobQueue.h"

#include <utility>

namespace jobq {

void JobQueue::push(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push(std::move(job));
    }

    condition_.notify_one();
}

std::optional<Job> JobQueue::tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (jobs_.empty()) {
        return std::nullopt;
    }

    Job job = std::move(jobs_.front());
    jobs_.pop();

    return job;
}

Job JobQueue::waitAndPop() {
    std::unique_lock<std::mutex> lock(mutex_);

    condition_.wait(lock, [this] {
        return !jobs_.empty();
    });

    Job job = std::move(jobs_.front());
    jobs_.pop();

    return job;
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