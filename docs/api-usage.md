# C++ API Developer Guide

This guide demonstrates how to embed and use JobQ within your own modern C++20 applications. All examples use actual types and function signatures from the JobQ headers.

---

## 1. Quick Start: Queue, Handlers, and Workers

The minimal setup requires a `JobQueue`, a `JobExecutor`, and a `WorkerPool`:

```cpp
#include <iostream>
#include <thread>
#include <chrono>

#include "executor/JobExecutor.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

int main() {
    using namespace jobq;

    // 1. Instantiate the shared queue and executor
    JobQueue queue;
    JobExecutor executor;

    // 2. Register job handlers by type name
    executor.registerHandler("GREETING", [](const Job& job) {
        std::cout << "[Handler] Hello, " << job.getPayload() << "!\n";
    });

    executor.registerHandler("CALCULATION", [](const Job& job) {
        std::cout << "[Handler] Computing payload: " << job.getPayload() << "\n";
    });

    // 3. Start a pool of 2 worker threads
    WorkerPool pool(queue, executor, 2);
    pool.start();

    // 4. Create a JobManager and submit tasks
    JobManager manager(queue);
    manager.submit("GREETING", "Alice", JobPriority::Normal);
    manager.submit("CALCULATION", "42 * 2", JobPriority::High);

    // 5. Allow workers to process tasks
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 6. Stop worker pool gracefully (closes queue and joins threads)
    pool.stop();

    return 0;
}
```

---

## 2. Priority Scheduling & Exponential Backoff Retries

Jobs can be assigned priorities and customized retry policies:

```cpp
#include <chrono>
#include <iostream>
#include "manager/JobManager.h"
#include "queue/JobQueue.h"

void submitWithRetries(jobq::JobManager& manager) {
    using namespace jobq;

    // Configure retry parameters:
    // - Up to 3 retries
    // - Initial delay: 200 ms
    // - Multiplier: 2.0x (delays: 200ms -> 400ms -> 800ms)
    // - Maximum delay capped at 5000 ms
    RetryPolicy policy{
        .maxRetries = 3,
        .initialDelay = std::chrono::milliseconds(200),
        .multiplier = 2.0,
        .maxDelay = std::chrono::milliseconds(5000)
    };

    // High priority job with exponential backoff
    auto job = manager.submit(
        "NETWORK_SYNC",
        R"({"endpoint": "https://api.example.com/sync"})",
        JobPriority::High,
        policy
    );

    std::cout << "Submitted Job ID: " << job->getId()
              << " with " << policy.maxRetries << " retries.\n";
}
```

---

## 3. Persistent Storage & Crash Recovery

To persist jobs to disk so that unfinished tasks survive crashes or restarts, attach a `FileJobStorage` to `JobManager`:

```cpp
#include <memory>
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"

void runWithPersistence() {
    using namespace jobq;

    JobQueue queue;

    // 1. Create a file-backed storage instance
    auto storage = std::make_shared<FileJobStorage>("data/jobs.journal");

    // 2. Pass storage to JobManager; it automatically recovers uncompleted jobs
    JobManager manager(queue, storage);

    std::cout << "Recovered " << manager.totalJobs()
              << " jobs from existing journal.\n";

    // 3. New submissions automatically persist to disk
    manager.submit("REPORT", "daily-revenue", JobPriority::Normal);
}
```

---

## 4. Collecting & Exporting Runtime Metrics

Inject `JobMetrics` into `JobManager` and `WorkerPool` to collect execution statistics and export them as formatted text or structured JSON:

```cpp
#include <iostream>
#include <memory>
#include "executor/JobExecutor.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "metrics/JobMetricsJson.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

void runWithMetrics() {
    using namespace jobq;

    // Shared metrics collector
    auto metrics = std::make_shared<JobMetrics>();

    JobQueue queue;
    JobExecutor executor;
    executor.registerHandler("TASK", [](const Job&) {
        // Business logic here
    });

    // Inject metrics into JobManager and WorkerPool
    JobManager manager(queue, nullptr, metrics);
    WorkerPool pool(queue, executor, 4, metrics);

    pool.start();

    // Submit work
    for (int i = 0; i < 50; ++i) {
        manager.submit("TASK", std::to_string(i));
    }

    // Stop workers
    pool.stop();

    // 1. Access point-in-time snapshot
    JobMetricsSnapshot snapshot = metrics->snapshot();

    // 2. Format as human-readable table
    std::cout << formatMetrics(snapshot) << "\n";

    // 3. Format as structured JSON
    std::string jsonOutput = toJson(snapshot);
    std::cout << jsonOutput << "\n";
}
```

---

## 5. Querying Job Status

You can query individual jobs by ID at any time:

```cpp
#include <iostream>
#include "manager/JobManager.h"

void inspectJob(const jobq::JobManager& manager, uint64_t jobId) {
    auto status = manager.getStatus(jobId);
    if (!status.has_value()) {
        std::cout << "Job " << jobId << " not found.\n";
        return;
    }

    std::cout << "Job " << jobId << " status: "
              << jobq::toString(*status) << "\n";

    auto job = manager.getJob(jobId);
    if (job) {
        std::cout << "  Attempts made: " << job->getAttemptsMade() << "\n";
        std::cout << "  Payload:       " << job->getPayload() << "\n";
    }
}
```
