#include "queue/JobQueue.h"

#include <utility>

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

        jobs_.push(
            QueueEntry{
                std::move(job),
                nextSequence_++
            }
        );
    }

    condition_.notify_one();
    return true;
}

bool JobQueue::pushRetry(
    std::shared_ptr<Job> job,
    std::chrono::milliseconds delay
) {
    if (!job) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (delay <= std::chrono::milliseconds::zero()) {
            jobs_.push(
                QueueEntry{
                    std::move(job),
                    nextSequence_++
                }
            );
        } else {
            delayedRetries_.push(
                DelayedEntry{
                    std::move(job),
                    nextSequence_++,
                    Clock::now() + delay
                }
            );
        }

    }

    condition_.notify_all();
    return true;
}

void JobQueue::promoteDueRetriesLocked() {
    const auto now = Clock::now();

    while (!delayedRetries_.empty() &&
           delayedRetries_.top().readyAt <= now) {
        auto entry = delayedRetries_.top();
        delayedRetries_.pop();

        jobs_.push(
            QueueEntry{
                std::move(entry.job),
                nextSequence_++
            }
        );
    }
}

std::shared_ptr<Job> JobQueue::tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);

    promoteDueRetriesLocked();

    if (jobs_.empty()) {
        return nullptr;
    }

    auto job = jobs_.top().job;
    jobs_.pop();

    return job;
}

std::shared_ptr<Job> JobQueue::waitAndPop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        promoteDueRetriesLocked();

        if (!jobs_.empty()) {
            auto job = jobs_.top().job;
            jobs_.pop();

            return job;
        }

        if (closed_ && delayedRetries_.empty()) {
            return nullptr;
        }

        if (!delayedRetries_.empty()) {
            condition_.wait_until(
                lock,
                delayedRetries_.top().readyAt
            );
        } else {
            condition_.wait(lock);
        }
    }
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

    return jobs_.empty() &&
           delayedRetries_.empty();
}

std::size_t JobQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return jobs_.size() +
           delayedRetries_.size();
}

}