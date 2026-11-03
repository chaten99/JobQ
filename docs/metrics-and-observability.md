# Metrics & Observability

JobQ includes an integrated, thread-safe runtime metrics and observability subsystem that measures throughput, failure rates, retry volumes, and execution latency.

---

## 1. Metrics Architecture

The metrics subsystem is centered around `JobMetrics` (`include/metrics/JobMetrics.h`), an in-memory accumulator tracking high-precision performance data across worker threads:

```mermaid
flowchart TD
    subgraph Producers ["Producers / JobManager"]
        JM["JobManager::submit()"] -->|recordSubmission()| M["JobMetrics"]
    end

    subgraph Workers ["WorkerPool Threads"]
        W["Worker::run()"] -->|recordStartedExecution()| M
        W -->|recordCompletedExecution(duration)| M
        W -->|recordFailedExecution(duration)| M
        W -->|recordScheduledRetry()| M
    end

    subgraph Exporters ["Exporters & Consumers"]
        M -->|snapshot()| SNAP["JobMetricsSnapshot (Immutable)"]
        SNAP -->|formatMetrics() / printMetrics()| TEXT["Human-Readable Text Table"]
        SNAP -->|toJson()| JSON["Structured JSON (Schema v1)"]
    end
```

---

## 2. Counter Semantics & Definitions

To interpret metrics correctly, it is important to understand the distinctions between tasks and attempts:

| Metric Field | Type | Description & Semantics |
|---|---|---|
| `submittedJobs` | `uint64_t` | Total count of distinct jobs submitted to `JobManager`. |
| `startedExecutions` | `uint64_t` | Total number of times a worker thread has started running a handler attempt. |
| `completedExecutions` | `uint64_t` | Total number of handler executions that finished successfully without throwing. |
| `failedExecutions` | `uint64_t` | Total number of handler executions that threw an exception. |
| `scheduledRetries` | `uint64_t` | Total number of retry attempts successfully enqueued into the delayed retry queue. |
| `finishedExecutions` | `uint64_t` | Total finished execution attempts (`completedExecutions + failedExecutions`). |

### Critical Semantic Distinctions
1. **Submissions vs. Executions:** If a job fails twice and succeeds on the third attempt, `submittedJobs = 1`, `startedExecutions = 3`, `failedExecutions = 2`, `completedExecutions = 1`, and `scheduledRetries = 2`.
2. **Scheduled Retries vs. Executed Retries:** `scheduledRetries` increments when the retry is placed into the queue. `startedExecutions` increments when a worker actually begins executing that retry.
3. **In-Memory Lifetime:** Metrics are accumulated in RAM during execution. Historical metrics do not survive process restarts.

---

## 3. Performance & Timing Measurements

All time measurements use `std::chrono::steady_clock` to ensure monotonic progression unaffected by system time adjustments or NTP synchronization.

| Timing Field | Units | Derivation / Formula |
|---|---|---|
| `totalExecutionTime` | `std::chrono::nanoseconds` | Cumulative sum of all completed and failed handler runtimes. |
| `averageExecutionTime` | `std::chrono::nanoseconds` | $\frac{\text{totalExecutionTime}}{\text{finishedExecutions}}$ (or `0` if `finishedExecutions == 0`). |
| `elapsedTime` | `std::chrono::nanoseconds` | Monotonic time elapsed since `JobMetrics` was instantiated or started. |
| `throughputJobsPerSecond` | `double` | $\frac{\text{completedExecutions}}{\text{elapsedTime in seconds}}$ (or `0.0` if `elapsedTime == 0`). |

---

## 4. Structured JSON Export

JobQ provides standard-compliant JSON serialization through `jobq::toJson(const JobMetricsSnapshot&)` in [`include/metrics/JobMetricsJson.h`](../include/metrics/JobMetricsJson.h).

### JSON Schema (Version 1)
```json
{
  "schemaVersion": 1,
  "metrics": {
    "submittedJobs": 100,
    "startedExecutions": 120,
    "completedExecutions": 90,
    "failedExecutions": 30,
    "scheduledRetries": 20,
    "finishedExecutions": 120
  },
  "performance": {
    "totalExecutionTimeNs": 2500000000,
    "averageExecutionTimeNs": 20833333,
    "elapsedTimeNs": 5000000000,
    "throughputJobsPerSecond": 18.0
  }
}
```

### JSON Serialization Rules
- **No External Dependencies:** Implemented purely in standard C++20.
- **Nanosecond Precision:** Duration fields are expressed as raw integer nanoseconds (`...Ns`) to avoid precision loss from floating-point milliseconds.
- **Numeric Values:** Metric counters and durations are serialized as numbers, not quoted strings.
- **Classic Locale:** Stream formatting is imbued with `std::locale::classic()` so decimal points remain `.` regardless of system locale (e.g. European commas).
- **Non-Finite Handling:** If `throughputJobsPerSecond` evaluates to `NaN` or `+/-Infinity` (e.g., zero elapsed time), it is safely serialized as JSON `null` rather than generating non-standard tokens.

---

## 5. Thread Safety Invariants

`JobMetrics` is designed for high-concurrency environments:
- Mutex-guarded critical sections protect latency accumulations and snapshot generation.
- Atomic counters protect individual metric updates.
- Taking a snapshot (`metrics.snapshot()`) returns a fully consistent, point-in-time immutable copy:
  ```cpp
  JobMetricsSnapshot snapshot = metrics.snapshot();
  ```
  This snapshot can be safely formatted or serialized on another thread without holding locks or blocking workers.
