#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "job/Job.h"

namespace jobq {

class JobQueue {
public:
    JobQueue() = default;
    ~JobQueue() = default;

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    bool push(std::shared_ptr<Job> job);

    bool pushRetry(
        std::shared_ptr<Job> job,
        std::chrono::milliseconds delay
    );

    std::shared_ptr<Job> tryPop();

    std::shared_ptr<Job> waitAndPop();

    void close();

    bool isClosed() const;

    bool empty() const;

    std::size_t size() const;

private:
    using Clock = std::chrono::steady_clock;

    struct QueueEntry {
        std::shared_ptr<Job> job;
        uint64_t sequence;
    };

    struct QueueEntryCompare {
        bool operator()(
            const QueueEntry& left,
            const QueueEntry& right
        ) const {
            const auto leftPriority =
                static_cast<int>(left.job->getPriority());

            const auto rightPriority =
                static_cast<int>(right.job->getPriority());

            if (leftPriority != rightPriority) {
                return leftPriority < rightPriority;
            }

            return left.sequence > right.sequence;
        }
    };

    struct DelayedEntry {
        std::shared_ptr<Job> job;
        uint64_t sequence;
        Clock::time_point readyAt;
    };

    struct DelayedEntryCompare {
        bool operator()(
            const DelayedEntry& left,
            const DelayedEntry& right
        ) const {
            if (left.readyAt != right.readyAt) {
                return left.readyAt > right.readyAt;
            }

            return left.sequence > right.sequence;
        }
    };

    void promoteDueRetriesLocked();

    std::priority_queue<
        QueueEntry,
        std::vector<QueueEntry>,
        QueueEntryCompare
    > jobs_;

    std::priority_queue<
        DelayedEntry,
        std::vector<DelayedEntry>,
        DelayedEntryCompare
    > delayedRetries_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;

    bool closed_{false};
    uint64_t nextSequence_{0};
};

}