#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

#include "executor/JobExecutor.h"
#include "queue/JobQueue.h"

namespace jobq {

class JobMetrics;

class Worker {
public:
    Worker(
        std::size_t id,
        JobQueue& queue,
        JobExecutor& executor,
        JobMetrics* metrics = nullptr
    );

    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();

    void join();

    bool isCurrentThread() const;

private:
    enum class State { Created, Running, Joined };

    void run();
    void processJob(const std::shared_ptr<Job>& job);

    std::size_t id_;

    JobQueue& queue_;
    JobExecutor& executor_;
    JobMetrics* metrics_{nullptr};

    mutable std::mutex threadMutex_;
    State state_{State::Created};
    std::thread thread_;

    std::atomic<std::thread::id> threadId_{};
};

} // namespace jobq
