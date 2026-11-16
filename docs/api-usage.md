# C++ API Developer Guide

This guide demonstrates how to embed and use JobQ within your own modern C++20 applications. All examples use actual types and function signatures from the JobQ headers.

## Public Engine API

Include `jobq/Engine.h` and link to `JobQ::JobQ` (`jobq_api`). This static library
owns the high-level API implementation and links publicly to `JobQ::Core`
(`jobq_core`). The Engine implementation privately owns a queue, executor,
manager, metrics collector, and worker pool. The existing lower-level interfaces
remain available through the core target.

For a source checkout embedded in an application's build, use the following CMake setup.

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApplication LANGUAGES CXX)
add_subdirectory(path/to/JobQ jobq-build EXCLUDE_FROM_ALL)
add_executable(my_application main.cpp)
target_link_libraries(my_application PRIVATE JobQ::JobQ)
```

The API has three public headers, `jobq/Engine.h`, `jobq/EngineConfig.h`, and
`jobq/JobOptions.h`. `Engine.h` includes the configuration and options headers.
`JobId` is a 64-bit unsigned identifier. `JobHandler` is
`std::function<void(const Job&)>`, matching the existing core signature. Payloads
are `std::string` values. Submission accepts strings by value and moves them into
the core, so callers can move an owned payload. During a handler invocation the
job reference and its payload remain valid. Copy any data needed beyond that
invocation.

```cpp
#include <chrono>
#include <iostream>
#include "jobq/Engine.h"

int main() {
    jobq::Engine engine({.workerCount = 4});
    engine.registerHandler("EMAIL", [](const jobq::Job& job) {
        std::cout << "Email to " << job.getPayload() << '\n';
    });
    engine.registerHandler("REPORT", [](const jobq::Job& job) {
        std::cout << "Report " << job.getPayload() << '\n';
    });

    engine.start();
    const auto id = engine.submit("EMAIL", "reader@example.com", {
        .priority = jobq::JobPriority::High,
        .retryPolicy = {
            .maxRetries = 2,
            .initialDelay = std::chrono::milliseconds(100),
            .multiplier = 2.0,
            .maxDelay = std::chrono::milliseconds(1000)
        }
    });
    engine.submit("REPORT", "monthly");
    engine.stop();

    const auto status = engine.getStatus(id);
    if (status) {
        std::cout << jobq::toString(*status) << '\n';
    }
    std::cout << engine.metrics().completedExecutions << '\n';
    return status == jobq::JobStatus::Completed ? 0 : 1;
}
```

### Configuration and ownership

`EngineConfig` defaults to four workers and in-memory operation. `workerCount`
must be positive. Set `storagePath` to a journal file to use the existing
`FileJobStorage` persistence and recovery behavior.

```cpp
jobq::Engine engine({
    .workerCount = 2,
    .storagePath = "data/application.journal"
});
```

Construction loads the journal and restores job status. Register handlers for
recovered job types before calling `start()`. Pending recovered work starts with
the worker pool. A recovered type with no registered handler follows the core's
execution failure and retry policy. Recovered work contributes to execution
metrics, while `submittedJobs` counts new submissions made through this engine.
Journal errors propagate from construction or submission.

The Engine owns its components and stored callable copies. It is neither copyable
nor movable. Captured references must outlive all handler invocations. Shared
captures can own resources needed by handlers. Keep the Engine's owner outside
its handlers, and finish concurrent member calls before destroying it. Destroying
an Engine in one of its own handlers cannot join that worker and terminates the
process. A destructor shutdown error also terminates, following its `noexcept`
contract. Use an explicit external `stop()` when shutdown errors must be caught.

### Lifecycle and errors

| Operation | Behavior |
|---|---|
| Construction | Creates components and loads configured storage. Workers wait for `start()`. |
| `registerHandler(type, handler)` | Allowed before startup. Empty types/callables and duplicate types throw `std::invalid_argument`. |
| `start()` | Starts workers once. Repeated startup and startup after shutdown throw `std::logic_error`. Startup failure closes the engine and propagates the error. |
| `submit(type, payload, options)` | Requires a running engine. Returns a `JobId`. Unknown types throw `std::invalid_argument` before creating a job. |
| `stop()` | Closes submissions, drains accepted jobs and scheduled retries, and joins workers. Repeated and concurrent calls are safe. |
| `stop()` before `start()` | Closes the engine. Recovered queued work remains in the journal for future recovery. |
| Destruction | Calls `stop()` and releases owned components and callables after worker cleanup. |
| `getStatus(id)` | Returns `std::optional<JobStatus>`, empty for an unknown ID. Available before startup and after shutdown. |
| `metrics()` | Returns a consistent `JobMetricsSnapshot` using the existing core semantics. |
| `isRunning()` | Reports whether startup completed and shutdown has not begun. |

Registration during or after startup and submission before startup or during/after
shutdown throw `std::logic_error`. The core validates priority and retry options
and propagates `std::invalid_argument` for invalid values. Handlers may run before
`start()` returns when recovery queued work. Submissions require completed
startup. Concurrent startup/shutdown is coordinated by the Engine.

Handlers can query status and metrics and submit further jobs while the engine
runs. Calling `stop()` from the same Engine's handler throws `std::logic_error`
before it can wait for its own worker. During external shutdown, new submissions
from handlers are rejected along with other submissions. Shutdown waits for
handlers to return, including their configured retry attempts and delays.

Handler exceptions use the existing worker isolation and retry behavior.
Inspect final status and failed/retry execution counters to observe outcomes.
The core's worker diagnostics continue to report execution and persistence
failures. Status can reach a terminal state before the worker's final metrics
update. After `stop()` returns, worker execution and metrics updates are drained.

The API adds no cancellation, restart, distributed coordination, or ABI-versioning
contract. It retains the core's in-process scheduling and journal semantics.
Registered jobs remain in the manager's registry for the Engine's lifetime.

Build and run the standalone example using `cmake --build build --target
jobq_public_api_example` and `./build/jobq_public_api_example.exe` on Windows.
The example target links to `JobQ::API` and compiles its own application source.

---

## 1. Lower-level Queue, Handlers, and Workers

For direct control of the lower-level components, construct a `JobQueue`, a `JobExecutor`, and a `WorkerPool`:

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
