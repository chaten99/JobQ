#include "queue/JobQueue.h"

#include <utility>

#include "metrics/MeasuredLock.h"

namespace jobq {

bool JobQueue::push(std::shared_ptr<Job> job) {
    if (!job) {
        return false;
    }

    {
        MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);

        if (closed_) {
            return false;
        }

        job->setQueueReadyTime(Clock::now());
        const auto priority = job->getPriority();

        jobs_.push(
            QueueEntry{
                std::move(job),
                nextSequence_++,
                priority
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
        MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);

        if (delay <= std::chrono::milliseconds::zero()) {
            job->setQueueReadyTime(Clock::now());
            const auto priority = job->getPriority();

            jobs_.push(
                QueueEntry{
                    std::move(job),
                    nextSequence_++,
                    priority
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
        delayedRetries_.top().job->setQueueReadyTime(
            delayedRetries_.top().readyAt
        );

        jobs_.push(
            QueueEntry{
                delayedRetries_.top().job,
                nextSequence_,
                delayedRetries_.top().job->getPriority()
            }
        );

        ++nextSequence_;
        delayedRetries_.pop();
    }
}

std::shared_ptr<Job> JobQueue::tryPop() {
    MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);

    promoteDueRetriesLocked();

    if (jobs_.empty()) {
        return nullptr;
    }

    auto job = jobs_.top().job;
    jobs_.pop();

    return job;
}

std::shared_ptr<Job> JobQueue::waitAndPop() {
    MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);

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

        lock.pauseHold();

        if (!delayedRetries_.empty()) {
            condition_.wait_until(
                lock.native(),
                delayedRetries_.top().readyAt
            );
        } else {
            condition_.wait(lock.native());
        }

        lock.resumeHold();
    }
}

void JobQueue::close() {
    {
        MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);
        closed_ = true;
    }

    condition_.notify_all();
}

bool JobQueue::isClosed() const {
    MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);
    return closed_;
}

bool JobQueue::empty() const {
    MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);

    return jobs_.empty() &&
           delayedRetries_.empty();
}

std::size_t JobQueue::size() const {
    MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobQueue);

    return jobs_.size() +
           delayedRetries_.size();
}

void JobQueue::setContentionMetrics(ContentionMetrics* metrics) noexcept {
    contention_ = metrics;
}

}
