#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "jobq/EngineConfig.h"
#include "jobq/EngineSnapshot.h"
#include "jobq/JobOptions.h"
#include "metrics/JobMetrics.h"

namespace jobq {

using JobId = std::uint64_t;
using JobHandler = std::function<void(const Job&)>;

class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine() noexcept;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void registerHandler(std::string type, JobHandler handler);

    void start();
    void stop();

    JobId submit(std::string type, std::string payload, JobOptions options = {});

    std::optional<JobStatus> getStatus(JobId id) const;
    std::optional<JobSnapshot> getJob(JobId id) const;
    std::vector<JobSnapshot> listJobs() const;
    JobMetricsSnapshot metrics() const;
    bool isRunning() const;

    EngineSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
