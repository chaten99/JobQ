#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "job/Job.h"

namespace jobq {

class ContentionMetrics;

class JobExecutor {
public:
    using JobHandler = std::function<void(const Job&)>;

    JobExecutor() = default;
    ~JobExecutor() = default;

    JobExecutor(const JobExecutor&) = delete;
    JobExecutor& operator=(const JobExecutor&) = delete;

    void setContentionMetrics(ContentionMetrics* metrics) noexcept;

    void registerHandler(std::string type, JobHandler handler);

    void execute(const Job& job) const;

private:
    ContentionMetrics* contention_{nullptr};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, JobHandler> handlers_;
};

} // namespace jobq