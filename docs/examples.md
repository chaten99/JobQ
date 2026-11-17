# Examples & Demonstrations

JobQ includes reference demonstrations and benchmark harnesses to verify system behavior under load.

## Public API developer examples

The examples below include `jobq/Engine.h` and link to `JobQ::JobQ`.
They use the Engine's public configuration, handler, submission, status, and
metrics interfaces. Build them with the default `JOBQ_BUILD_EXAMPLES=ON` setting.

```powershell
cmake -S . -B build -DJOBQ_BUILD_EXAMPLES=ON
cmake --build build --target jobq_example_basic_usage jobq_example_multiple_handlers jobq_example_job_options jobq_example_error_handling
ctest --test-dir build -R jobq_example --output-on-failure
```

| Example source | Target | Workflow |
|---|---|---|
| `examples/basic-usage/main.cpp` | `jobq_example_basic_usage` | Registers an email handler, submits a payload, receives it through a bounded future wait, and checks final status after shutdown. |
| `examples/multiple-handlers/main.cpp` | `jobq_example_multiple_handlers` | Routes email, report, and calculation jobs to separate handlers and verifies each result. |
| `examples/job-options/main.cpp` | `jobq_example_job_options` | Sets High priority and a bounded exponential retry policy for a transient sync failure. |
| `examples/error-handling/main.cpp` | `jobq_example_error_handling` | Catches configuration, registration, unknown-type, and lifecycle errors and inspects a handler failure through status and metrics. |

Each executable exits with a nonzero code if its verification fails. CTest runs
them with a 15-second process timeout. Execute them directly from the build
directory to see their output. The existing `jobq_public_api_example` remains
available. `JOBQ_BUILD_EXAMPLES=OFF` disables these public API example targets
and their CTest entries. Tests remain independently controlled by
`BUILD_TESTING`. The existing lower-level stress target and benchmarks retain
their own targets.

A minimal application owns its result state before constructing the Engine so
that handler captures remain valid through worker shutdown.

```cpp
#include <atomic>
#include "jobq/Engine.h"

int main() {
    std::atomic<unsigned> completed{0};
    jobq::Engine engine({.workerCount = 2});
    engine.registerHandler("REPORT", [&](const jobq::Job&) {
        ++completed;
    });
    engine.start();
    const auto id = engine.submit("REPORT", "monthly-sales");
    engine.stop();
    return completed == 1 &&
           engine.getStatus(id) == jobq::JobStatus::Completed ? 0 : 1;
}
```

Register handlers before startup and submit while running. `stop()` drains
accepted jobs, including scheduled retries, then joins workers. It provides the
completion boundary used by the multiple-handler, job-options, and error examples.
The basic example additionally observes the handler's result through a future.
Handler exceptions trigger the configured retry policy and eventually appear as
Completed or Failed status. Validation and lifecycle errors are raised at the
application call site and can be caught as `std::invalid_argument`,
`std::logic_error`, or storage-related exceptions.

`JobOptions` exposes priority and retry policy. Backoff delays apply to retries.
Initial submission delay, cancellation, restart, and a public completion-wait API
are not exposed. The examples use application synchronization and graceful
shutdown. See the [API guide](api-usage.md) for ownership and lifecycle details.

For separate projects, `tests/package_consumer` verifies the build-tree package
and `examples/installed-consumer` verifies an installed package. Both use
`find_package(JobQ CONFIG REQUIRED)` with a compatible version request and link
to `JobQ::JobQ`. The installed consumer can be copied outside the checkout.

```powershell
cmake --build build --target jobq_api
cmake --install build --prefix C:/temp/jobq-prefix
cmake -S examples/installed-consumer -B C:/temp/jobq-consumer -DCMAKE_PREFIX_PATH=C:/temp/jobq-prefix
cmake --build C:/temp/jobq-consumer
ctest --test-dir C:/temp/jobq-consumer --output-on-failure
```

Choose a dedicated prefix and use a matching compiler and configuration. See the
[package guide](cmake-packaging.md) for build-tree discovery and relocation.

## Public API test coverage

`tests/public_api/engine_test.cpp` contains the existing Engine lifecycle,
registration, priority, retry, concurrency, ownership, and recovery suite.
`tests/public_api/edge_cases_test.cpp` adds binary/empty payload checks,
case-sensitive handler routing, proof of configured worker concurrency,
non-standard exception retry exhaustion, rejected retry options, and recovery
from a journal write failure during submission. These tests use public methods
and application-owned synchronization. Existing recovery fixtures use the
supported core journal serialization format to seed persisted jobs.

```powershell
ctest --test-dir build -R jobq_public_api --output-on-failure
```

The API suites have 30-second CTest process timeouts. New asynchronous checks
use bounded futures or the existing timed condition-variable helpers, and final
status/metric assertions follow graceful shutdown.

---

## 1. Concurrency Stress Test (`jobq_stress_test`)

### Overview
- **File Location:** [`examples/stress_test.cpp`](../examples/stress_test.cpp)
- **Target Name:** `jobq_stress_test`
- **Purpose:** Verifies thread safety, priority queue integrity, and worker contention under heavy concurrent workloads.

### What It Demonstrates
- Multi-producer, multi-consumer concurrency: Multiple producer threads simultaneously submit hundreds of jobs with varying priorities (`High`, `Normal`, `Low`).
- Concurrent worker pool: Multiple worker threads dequeue and process jobs simultaneously without race conditions or deadlocks.
- Ordering verification: High-priority jobs are prioritized under contention.
- Clean shutdown: Coordinated queue closure and thread joining after queue drainage.

### How to Build
```powershell
cmake --build build --target jobq_stress_test
```

### How to Run
```powershell
.\build\jobq_stress_test.exe
```

### Sample Output
```text
=== JobQ Concurrency Stress Test ===
Starting 4 worker threads...
Starting 4 producer threads submitting 500 jobs each...
Producers finished submitting 2000 jobs.
Waiting for workers to drain queue...
All jobs executed successfully.
Queue size: 0
Worker pool stopped cleanly.
=== Test Completed Successfully ===
```

---

## 2. In-Process Runtime Demo (`jobq`)

### Overview
- **File Location:** [`src/main.cpp`](../src/main.cpp)
- **Target Name:** `jobq`
- **Purpose:** Demonstrates full integration of `JobManager`, `JobExecutor`, `WorkerPool`, exponential backoff retries, and `JobMetrics` reporting.

### What It Demonstrates
- Creating handlers for different job types (e.g. `EMAIL`, `REPORT`).
- Simulating transient failure with a retry policy (demonstrating backoff delays).
- Collecting execution timings with `JobMetrics`.
- Outputting formatted metrics tables at shutdown.

### How to Build
```powershell
cmake --build build --target jobq
```

### How to Run
```powershell
.\build\jobq.exe
```

### Sample Output
```text
=== JobQ In-Process Demo ===
[Worker 1] picked Job 1 (EMAIL, priority HIGH, attempt 1)
[Handler] Sending email to user@example.com
[Worker 1] Job 1 completed after 1 attempt(s)
[Worker 2] picked Job 2 (REPORT, priority NORMAL, attempt 1)
[Handler] Simulating network glitch for REPORT...
[Worker 2] Handler failed, scheduling retry in 100ms...
[Worker 1] picked Job 2 (REPORT, priority NORMAL, attempt 2)
[Handler] Generating report...
[Worker 1] Job 2 completed after 2 attempt(s)

========== JobQ Runtime Metrics ==========

Job Statistics

  Submitted Jobs:          2
  Started Executions:      3
  Completed Executions:    2
  Failed Executions:       1
  Scheduled Retries:       1
  Finished Executions:     3

Performance Statistics

  Total Execution Time:    25.40 ms
  Average Execution Time:  8.47 ms
  Elapsed Time:            152.18 ms
  Throughput:              13.14 jobs/sec

==========================================
```
