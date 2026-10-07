#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>

#include "job/Job.h"

namespace jobq {

class JobQueue {
public:
    JobQueue() = default;
    ~JobQueue() = default;

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    bool push(Job job);

    std::optional<Job> tryPop();

    std::optional<Job> waitAndPop();

    void close();

    bool isClosed() const;

    bool empty() const;

    std::size_t size() const;

private:
    std::queue<Job> jobs_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;

    bool closed_{false};
};

} // namespace jobq