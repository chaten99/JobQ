# Retry System & Exponential Backoff

JobQ provides automatic failure handling and delayed retries with configurable exponential backoff to handle transient execution errors gracefully.

---

## 1. RetryPolicy Configuration

Each job can be configured with an optional `RetryPolicy` during creation:

```cpp
struct RetryPolicy {
    std::size_t maxRetries{0};
    std::chrono::milliseconds initialDelay{100};
    double multiplier{2.0};
    std::chrono::milliseconds maxDelay{5000};
};
```

| Field | Type | Default | Description |
|---|---|---|---|
| `maxRetries` | `std::size_t` | `0` | Maximum number of retry attempts permitted after the initial failure. A value of `0` means no retries are attempted. |
| `initialDelay` | `std::chrono::milliseconds` | `100ms` | Delay duration before the first retry attempt is scheduled. |
| `multiplier` | `double` | `2.0` | Exponential factor applied to the delay on each subsequent retry. |
| `maxDelay` | `std::chrono::milliseconds` | `5000ms` | Upper bound clamp for the computed backoff delay. |

---

## 2. Execution Attempts vs. Submitted Jobs

It is essential to distinguish between a **submitted job** and an **execution attempt**:

- **Submitted Job:** Represents the single logical task submitted to JobQ (identified by its unique `uint64_t id`).
- **Execution Attempt:** An individual invocation of the registered handler by a worker.

If a job is configured with `maxRetries = 3`:
- Initial execution: Attempt 1 (`attemptsMade = 0` before it starts; `markRunning()` makes it `1`).
- If it fails: `markFailed()`, then `canRetry()` (`attemptsMade <= maxRetries`) is true and `prepareForRetry()` returns the job to `Pending`.
- First retry: Attempt 2 (`attemptsMade = 2` once running).
- Second retry: Attempt 3 (`attemptsMade = 3`).
- Third retry: Attempt 4 (`attemptsMade = 4`).
- If attempt 4 fails: `canRetry()` returns `false` (`4 > 3`), and the job stays `Failed`.

Total executions can equal up to `1 + maxRetries`.

---

## 3. Backoff Delay Calculation

When a failure occurs, the delay before the next attempt is calculated using an exponential backoff formula clamped by `maxDelay`:

$$\text{delay} = \min\left(\text{initialDelay} \times \text{multiplier}^{\text{attemptsMade}},\, \text{maxDelay}\right)$$

In C++:
```cpp
std::chrono::milliseconds Job::getRetryDelay() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (retryPolicy_.maxRetries == 0) {
        return std::chrono::milliseconds(0);
    }

    const double factor = std::pow(
        retryPolicy_.multiplier,
        static_cast<double>(attemptsMade_)
    );

    const auto calculatedMs = static_cast<int64_t>(
        static_cast<double>(retryPolicy_.initialDelay.count()) * factor
    );

    const auto cappedMs = std::min(
        calculatedMs,
        static_cast<int64_t>(retryPolicy_.maxDelay.count())
    );

    return std::chrono::milliseconds(cappedMs);
}
```

### Example Delay Progression
With default settings (`initialDelay = 100ms`, `multiplier = 2.0`, `maxDelay = 5000ms`):

| Attempt Number | `attemptsMade_` | Computed Delay | Effective Delay |
|---|---|---|---|
| Attempt 1 (Initial) | `0` | $100 \times 2.0^0 = 100\text{ ms}$ | **100 ms** |
| Attempt 2 (Retry 1) | `1` | $100 \times 2.0^1 = 200\text{ ms}$ | **200 ms** |
| Attempt 3 (Retry 2) | `2` | $100 \times 2.0^2 = 400\text{ ms}$ | **400 ms** |
| Attempt 4 (Retry 3) | `3` | $100 \times 2.0^3 = 800\text{ ms}$ | **800 ms** |
| Attempt 5 (Retry 4) | `4` | $100 \times 2.0^4 = 1600\text{ ms}$ | **1600 ms** |
| Attempt 6 (Retry 5) | `5` | $100 \times 2.0^5 = 3200\text{ ms}$ | **3200 ms** |
| Attempt 7 (Retry 6) | `6` | $100 \times 2.0^6 = 6400\text{ ms}$ | **5000 ms (clamped)** |

---

## 4. Retry Workflow in Workers

When an execution error occurs in a worker thread:

```mermaid
sequenceDiagram
    participant W as Worker
    participant J as Job
    participant Q as JobQueue
    participant M as JobMetrics

    Note over W: Worker runs handler; exception thrown
    W->>J: canRetry()
    alt canRetry() == true
        W->>J: getRetryDelay()
        J-->>W: returns calculated delay (e.g. 200ms)
        W->>J: prepareForRetry()
        Note over J: attemptsMade++ <br/> status -> Pending
        W->>Q: pushRetry(job, delay)
        Note over Q: Inserts into delayed min-heap
        W->>M: recordScheduledRetry()
    else canRetry() == false
        W->>J: markFailed()
        Note over J: status -> Failed (Terminal)
    end
```

---

## 5. Retry Behavior Across Crash Recovery

When JobQ is configured with persistent storage (`FileJobStorage`):
- `attemptsMade` is serialized in the journal alongside the job status and retry policy.
- If the application crashes while a job is running or waiting in the retry queue:
  - Upon startup, `JobManager::restoreStoredJobs()` inspects the latest journal record.
  - Jobs in `Running`, `Failed` (with retries left) or `Pending` state are reloaded. An interrupted `Running` job counts as a failed attempt first.
  - The job retains its existing `attemptsMade` count.
  - If `attemptsMade <= maxRetries`, the job is re-enqueued as `Pending` through the delayed-retry path (`pushRetry()` with the backoff delay computed from its attempt count, restarted at recovery time) and is eligible for its remaining retries.
  - Once `attemptsMade > maxRetries` the job stays `Failed` and is not re-queued.
  - Previous attempts are never forgotten, preventing infinite retry loops across process restarts.
