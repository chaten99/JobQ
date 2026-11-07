# Job Queue & Worker Concurrency

This document details the multi-threaded scheduling architecture of JobQ, including priority ordering, FIFO tie-breaking, delayed retry min-heaps, worker thread loops, and clean shutdown mechanics.

---

## 1. The Scheduling Model

JobQ uses an in-memory priority queue protected by standard C++ synchronization primitives (`std::mutex`, `std::condition_variable`).

```mermaid
flowchart TD
    subgraph Producers ["Producers / Submitters"]
        P1["Thread 1"] -->|push()| JQ
        P2["Thread 2"] -->|push()| JQ
    end

    subgraph JQ ["JobQueue Internal Architecture"]
        subgraph ActiveQueue ["Active Priority Queue (jobs_)"]
            High["Priority: High (Sequence N)"]
            Normal1["Priority: Normal (Sequence 1)"]
            Normal2["Priority: Normal (Sequence 2)"]
            Low["Priority: Low (Sequence M)"]
        end

        subgraph DelayedQueue ["Delayed Min-Heap (delayedRetries_)"]
            D1["Retry Job A (readyAt: T0 + 200ms)"]
            D2["Retry Job B (readyAt: T0 + 800ms)"]
        end
    end

    D1 -.->|Due time reached| ActiveQueue

    subgraph Consumers ["WorkerPool Threads"]
        ActiveQueue -->|waitAndPop()| W1["Worker 1"]
        ActiveQueue -->|waitAndPop()| W2["Worker 2"]
        ActiveQueue -->|waitAndPop()| WN["Worker N"]
    end
```

---

## 2. Priority Scheduling & Deterministic FIFO Ordering

### The Challenge
Standard `std::priority_queue` does not guarantee FIFO order for elements with equal priority. If two jobs with `JobPriority::Normal` are pushed at different times, standard heaps may reorder them arbitrarily depending on tree structure.

### The Solution: Sequence Numbers
To preserve strict FIFO ordering among jobs of the same priority, `JobQueue` wraps jobs in a `QueueEntry` structure containing a monotonically increasing `sequence` counter:

```cpp
struct QueueEntry {
    std::shared_ptr<Job> job;
    uint64_t sequence;
};

struct QueueEntryCompare {
    bool operator()(const QueueEntry& left, const QueueEntry& right) const {
        const auto leftPriority = static_cast<int>(left.job->getPriority());
        const auto rightPriority = static_cast<int>(right.job->getPriority());

        if (leftPriority != rightPriority) {
            return leftPriority < rightPriority; // Higher priority popped first
        }

        return left.sequence > right.sequence;   // Lower sequence number popped first (FIFO)
    }
};
```

Whenever a job is pushed:
```cpp
bool JobQueue::push(std::shared_ptr<Job> job) {
    if (!job) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            return false;
        }

        jobs_.push(QueueEntry{std::move(job), nextSequence_++});
    }

    condition_.notify_one();
    return true;
}
```

This guarantees:
1. `High` priority jobs are **always** popped before `Normal` and `Low` jobs.
2. For jobs with identical priority, jobs submitted earlier are **always** popped before jobs submitted later.

---

## 3. Delayed Retry Scheduling

When a job fails and is eligible for retry, it must not be immediately retried if a backoff delay is specified.

### Architecture
- Delayed jobs are stored in `delayedRetries_`, a `std::priority_queue` sorted as a min-heap by `readyAt`:
  ```cpp
  struct DelayedEntry {
      std::shared_ptr<Job> job;
      uint64_t sequence;
      std::chrono::steady_clock::time_point readyAt;
  };
  ```
- No separate timer thread is required. The promotion of ready delayed entries is driven lazily and accurately within `waitAndPop()`.

### Timed Condition Variable Waiting
When worker threads call `waitAndPop()`:
1. `promoteDueRetriesLocked()` checks `delayedRetries_`. Any job whose `readyAt <= steady_clock::now()` is moved into `jobs_`.
2. If `jobs_` is non-empty, the top job is popped immediately.
3. If `jobs_` is empty but `delayedRetries_` contains jobs:
   - The thread waits on `condition_` using `wait_until()` set to the earliest `readyAt` time point.
4. If both queues are empty, the thread waits indefinitely until notified by a `push()`, `pushRetry()` or `close()`. If the queue is closed and both queues are empty, `nullptr` is returned instead.

Closure, ordering and concurrency guarantees are detailed in [Queue Reliability](queue-reliability.md).

---

## 4. Worker Execution Loop

Each `Worker` owns an independent `std::thread`. Its run loop consumes jobs until `waitAndPop()` returns `nullptr` (queue closed and fully drained). In simplified form:

```cpp
void Worker::run() {
    try {
        while (true) {
            auto job = queue_.waitAndPop();      // exceptions: logged, worker exits
            if (!job) break;                     // closed and drained

            try {
                processJob(job);                 // markRunning, execute, markCompleted,
            } catch (...) {                      // or fail + retry handling
                // unexpected error: logged with the job id, loop continues
            }
        }
    } catch (...) {
    }
}
```

Inside `processJob`, a handler exception marks the job `Failed` (`markFailed()`); if `canRetry()` is true, `prepareForRetry()` returns it to `Pending` and `pushRetry(job, delay)` re-queues it. A retry is counted only when `pushRetry()` returned true. See [Fault Tolerance](fault-tolerance.md) for the exact outcomes and [Worker Lifecycle](worker-lifecycle.md) for the thread-level behaviour.

### Key Safety Invariants
- **Worker Resilience:** Handlers throwing exceptions never terminate the worker thread. The per-job layer handles job failures; an outer layer logs anything unexpected.
- **Graceful Termination:** Workers only exit when `waitAndPop()` returns `nullptr`.

---

## 5. WorkerPool Lifecycle Management

The `WorkerPool` controls the lifecycles of multiple worker threads:

```cpp
class WorkerPool {
public:
    WorkerPool(JobQueue& queue, JobExecutor& executor, std::size_t workerCount);
    WorkerPool(JobQueue& queue, JobExecutor& executor, std::size_t workerCount, JobMetrics& metrics);
    ~WorkerPool();

    void start();                 // throws std::logic_error if already started or stopped
    void stop();                  // idempotent; throws std::logic_error from a worker thread
    std::size_t size() const;
    bool isStarted() const;
    bool isStopped() const;
};
```

### Coordinated Graceful Shutdown
1. `stop()` marks the pool as stopping and calls `queue_.close()`.
2. `JobQueue::close()` sets `closed_ = true` and notifies all waiting workers.
3. Workers keep consuming: every accepted ready job and every delayed retry is executed (retries scheduled during shutdown are accepted).
4. Workers see `waitAndPop() == nullptr` once both heaps are empty and exit.
5. `stop()` joins every worker thread and marks the pool stopped. Repeated and concurrent calls are safe.

The full policy, scenario table and limitations are in [Graceful Shutdown](graceful-shutdown.md).
