# JobQ System Architecture

JobQ is a multithreaded, in-process job queue and execution engine implemented in modern C++20. This document details the component architecture, execution pipelines, data flow, threading models, and subsystem interactions.

---

## 1. High-Level Architecture Overview

JobQ is structured around decoupled, single-responsibility components communicating through thread-safe queues and observer interfaces.

```mermaid
flowchart TD
    Client["Client / CLI / Application"] -->|submit / getJob| JM["JobManager"]

    subgraph StorageSubsystem ["Persistence & Observability Subsystem"]
        JS["JobStorage / FileJobStorage"]
        METRICS["JobMetrics (Thread-Safe)"]
    end

    subgraph CoreEngine ["Core Execution Engine"]
        JQ["JobQueue (Priority + Delayed Retries)"]
        WP["WorkerPool"]
        W1["Worker 1"]
        W2["Worker 2"]
        WN["Worker N"]
        JE["JobExecutor (Handler Registry)"]
    end

    JM -->|push| JQ
    JM -.->|StateObserver| JS
    JM -.->|recordSubmission| METRICS

    WP --> W1
    WP --> W2
    WP --> WN

    W1 -->|waitAndPop / pushRetry| JQ
    W2 -->|waitAndPop / pushRetry| JQ
    WN -->|waitAndPop / pushRetry| JQ

    W1 -->|execute| JE
    W2 -->|execute| JE
    WN -->|execute| JE

    W1 -.->|record execution & timing| METRICS
    W2 -.->|record execution & timing| METRICS
    WN -.->|record execution & timing| METRICS

    JM -.->|restore on startup| JS
```

---

## 2. Core Components & Responsibilities

| Component | Header / Source | Primary Responsibilities |
|---|---|---|
| **Job** | `include/job/Job.h`<br>`src/job/Job.cpp` | Encapsulates job identity (`id`), type, payload, priority, lifecycle status, retry policy, attempt counter, and state observer notification callback. |
| **JobQueue** | `include/queue/JobQueue.h`<br>`src/queue/JobQueue.cpp` | Thread-safe scheduling queue. Provides priority ordering with FIFO tie-breaking for equal priorities, delayed retry scheduling with timed condition variable waiting, and clean shutdown signaling. |
| **JobExecutor** | `include/executor/JobExecutor.h`<br>`src/executor/JobExecutor.cpp` | Function registry mapping job type strings to callable handlers (`std::function<void(const Job&)>`). Propagates handler failures as exceptions to workers. |
| **Worker** | `include/worker/Worker.h`<br>`src/worker/Worker.cpp` | Worker thread consumer. Consumes jobs from `JobQueue`, transitions status to `Running`, executes handler via `JobExecutor`, marks `Completed`, or schedules retries with exponential backoff on failure. |
| **WorkerPool** | `include/worker/WorkerPool.h`<br>`src/worker/WorkerPool.cpp` | Manages worker thread lifecycles: worker instantiation, thread startup (`start()`), and graceful stopping (`stop()`) by closing the queue and joining all worker threads. |
| **JobManager** | `include/manager/JobManager.h`<br>`src/manager/JobManager.cpp` | Top-level job orchestrator. Generates atomic monotonic job IDs, maintains in-memory job registry (`unordered_map` with `shared_mutex`), attaches state persistence observers, and manages startup crash recovery. |
| **FileJobStorage** | `include/storage/FileJobStorage.h`<br>`src/storage/FileJobStorage.cpp` | Append-only text journal persistence. Saves serialized `JobRecord` snapshots and recovers historical jobs on startup, automatically repairing truncated/incomplete tails from hard crashes. |
| **JobMetrics** | `include/metrics/JobMetrics.h`<br>`src/metrics/JobMetrics.cpp` | Thread-safe runtime metrics collector tracking job submissions, execution attempts, completions, failures, retries, monotonic execution durations, and real-time throughput. |
| **JobMetricsJson** | `include/metrics/JobMetricsJson.h`<br>`src/metrics/JobMetricsJson.cpp` | Serializes immutable `JobMetricsSnapshot` structs into valid JSON adhering to schema version 1 with nanosecond precision integers and safe floating-point throughput handling. |
| **CLI Engine** | `include/cli/CommandLine.h`<br>`src/cli/CommandHandlers.cpp` | Robust command line parser and execution handlers supporting subcommands (`submit`, `list`, `status`, `run`, `metrics`) with text and JSON output formats. |

---

## 3. Job Execution Lifecycle Flow

The complete flow of a job from submission through worker dispatch, handler execution, and final status reporting:

```mermaid
sequenceDiagram
    autonumber
    actor Client as Client / Producer
    participant JM as JobManager
    participant JQ as JobQueue
    participant WP as WorkerPool / Worker
    participant JE as JobExecutor
    participant JS as FileJobStorage
    participant MET as JobMetrics

    Client->>JM: submit(type, payload, priority, retryPolicy)
    activate JM
    JM->>JM: Allocate atomic ID & register in jobs_
    JM->>JS: persistSnapshot(Pending)
    JM->>MET: recordSubmission()
    JM->>JQ: push(job)
    deactivate JM

    activate WP
    WP->>JQ: waitAndPop()
    JQ-->>WP: return job
    WP->>WP: job->markRunning()
    WP->>JS: persistSnapshot(Running)
    WP->>MET: recordStartedExecution()

    WP->>JE: execute(*job)
    activate JE
    alt Execution Succeeded
        JE-->>WP: return clean
        WP->>WP: job->markCompleted()
        WP->>JS: persistSnapshot(Completed)
        WP->>MET: recordCompletedExecution(duration)
    else Execution Threw Exception
        JE-->>WP: throw std::runtime_error
        deactivate JE
        WP->>MET: recordFailedExecution(duration)
        alt canRetry() == true
            WP->>WP: job->prepareForRetry() (status -> Pending)
            WP->>JS: persistSnapshot(Pending)
            WP->>JQ: pushRetry(job, delay)
            WP->>MET: recordScheduledRetry()
        else canRetry() == false (Retries Exhausted)
            WP->>WP: job->markFailed()
            WP->>JS: persistSnapshot(Failed)
        end
    end
    deactivate WP
```

---

## 4. Retry and Backoff Subsystem

When a job handler fails during execution, JobQ invokes the retry subsystem rather than immediately abandoning the task:

```mermaid
flowchart TD
    Fail["Handler Execution Throws Exception"] --> Check{"job->canRetry()?<br>(attemptsMade < maxRetries)"}

    Check -->|No| MarkFailed["job->markFailed()"]
    MarkFailed --> PersistFail["Persist Status: Failed"]
    PersistFail --> EndFailed(["Job Terminated (Failed)"])

    Check -->|Yes| Prep["job->prepareForRetry()"]
    Prep --> IncAttempts["Increment attemptsMade_<br>Reset status_ to Pending"]
    IncAttempts --> CalcDelay["Calculate Exponential Backoff Delay:<br>delay = min(initialDelay * multiplier^attempts, maxDelay)"]
    CalcDelay --> PushDelayed["queue.pushRetry(job, delay)"]
    PushDelayed --> PersistPending["Persist Status: Pending"]
    PersistPending --> RecordRetry["metrics.recordScheduledRetry()"]
    RecordRetry --> WaitTimer["Job sleeps in delayed min-heap"]
    WaitTimer --> DueCheck{"Clock::now() >= readyAt?"}
    DueCheck -->|Yes| Promote["promoteDueRetriesLocked()<br>Move to active priority queue"]
    Promote --> WorkerPick["Worker picks job for re-execution"]
    WorkerPick --> ExecuteAgain["JobExecutor executes handler again"]
```

### Delayed Queue Mechanics
- `JobQueue` maintains two internal queues:
  1. `jobs_`: Active priority queue ready for immediate consumption.
  2. `delayedRetries_`: Min-heap ordered by `readyAt` (`std::chrono::steady_clock::time_point`).
- When a worker calls `waitAndPop()`:
  - Any delayed entries whose `readyAt <= steady_clock::now()` are automatically promoted to `jobs_`.
  - If `jobs_` is empty but `delayedRetries_` has pending items, the calling thread waits on `std::condition_variable` with a predicate timed to wake up at the earliest `readyAt`.
  - Avoids busy-waiting loops and thread spinning entirely.

---

## 5. Persistence and Recovery Architecture

JobQ uses an **append-only journal** approach for durable job records:

```mermaid
flowchart LR
    subgraph Runtime ["In-Memory Runtime"]
        Job["Job State Change"] --> Obs["JobStateObserver Callback"]
        Obs --> Ser["serializeJobRecord()"]
    end

    subgraph Storage ["Durable Disk Storage"]
        Ser --> Append["Append line to journal file"]
        Crash["Application Crash / Exit"]
    end

    subgraph Recovery ["Startup Recovery Pipeline"]
        Crash --> Restart["JobManager restart"]
        Restart --> Read["Read all journal lines"]
        Read --> Reconcile["Reconcile latest state per Job ID"]
        Reconcile --> EnqueuePending["Re-enqueue Pending / Running jobs"]
        Reconcile --> RestoreCompleted["Restore Completed / Failed jobs to registry"]
    end
```

### Crash Recovery Guarantees
- **Incomplete Tail Repair:** If the process is forcefully terminated midway through writing a line, `FileJobStorage` identifies and truncates the corrupted tail record during initialization.
- **Last-Write-Wins:** Because state transitions append new snapshots to the journal, reading from top to bottom results in the last record reflecting the final known state.
- **Interrupted Job Recovery:** Jobs that were `Running` at the time of a crash are restored back to `Pending` so that surviving retries or executions can be resumed upon restart.

---

## 6. Concurrency and Thread Safety Model

JobQ is engineered for safe multi-threaded concurrency without race conditions:

| Component | Synchronization Primitives | Invariants Protected |
|---|---|---|
| `Job` | `mutable std::mutex statusMutex_` | Thread-safe status transitions, attempt counting, and atomic snapshots even during concurrent inspection. |
| `JobQueue` | `mutable std::mutex mutex_`<br>`std::condition_variable notEmptyCv_` | Atomicity of priority heap pushes/pops, FIFO sequence ordering, delayed queue promotions, and graceful shutdown notification. |
| `JobManager` | `std::atomic<uint64_t> nextId_`<br>`mutable std::shared_mutex registryMutex_` | Lock-free unique ID generation; multiple concurrent readers (`getJob`, `getStatus`) with exclusive writer locks for submissions. |
| `JobExecutor` | `mutable std::mutex mutex_` | Safe handler registration and execution lookups. |
| `FileJobStorage` | `std::mutex mutex_` | Serialized disk write operations, preventing interleaved or corrupted journal lines across threads. |
| `JobMetrics` | `mutable std::mutex mutex_`<br>`std::atomic<uint64_t>` | Thread-safe execution counters, execution duration tracking, and immutable snapshot capture. |
