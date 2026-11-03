# Architectural Design Decisions

This document outlines the core technical decisions, design trade-offs, and architectural rationales evident throughout the JobQ codebase.

---

## 1. Choice of C++20

### Context
Building a robust multithreaded queue engine requires strong synchronization primitives, chrono utilities, and expressive value semantics.

### Decision
JobQ targets standard **C++20** (`set(CMAKE_CXX_STANDARD 20)`).

### Rationale
- Standardized synchronization tools such as `std::latch` and `std::shared_mutex` eliminate third-party dependencies (like Boost).
- Enhanced `<chrono>` utilities provide seamless time-point math and nanosecond duration precision.
- Modern `<filesystem>` simplifies cross-platform journal path management.
- Strong static type safety and move semantics minimize heap allocation overhead during job submission and dispatch.

---

## 2. In-Memory Thread-Safe Priority Queue with Sequence FIFO

### Context
Standard priority queues (`std::priority_queue`) do not preserve insertion order among items with equal priority.

### Decision
`JobQueue` embeds a 64-bit monotonically increasing `sequence` number in every `QueueEntry`. The comparator sorts first by `JobPriority` (descending) and second by `sequence` (ascending).

### Rationale
- High-priority jobs always take precedence.
- Equal-priority jobs strictly honor First-In, First-Out (FIFO) ordering.
- Embedding sequence counters eliminates the need to maintain multiple FIFO queues per priority tier.

---

## 3. Decoupling Queue Scheduling from Worker Execution

### Context
A job queue could directly trigger threads or callbacks upon insertion.

### Decision
`JobQueue`, `WorkerPool`, and `JobExecutor` are independent components:
- `JobQueue` knows only about thread-safe priority scheduling and delayed entry promotion.
- `JobExecutor` maintains a dictionary of string type names to callable handlers.
- `Worker` manages execution loops, exceptions, and status transitions.

### Rationale
- **Single Responsibility:** Queue logic is testable in total isolation from thread management and handler logic.
- **Worker Resilience:** Handlers that throw unhandled exceptions or fail are contained within `Worker::run()`. The worker stays alive and schedules retries rather than crashing the queue.

---

## 4. Min-Heap Delayed Retries with Timed Condition Variable Waiting

### Context
Scheduling delayed retries requires delaying a job until a future time point.

### Decision
Rather than spawning separate timer threads or sleeping inside workers, `JobQueue` holds delayed entries in a min-heap sorted by `readyAt`. When the ready queue is empty, the worker thread executes `wait_until()` on `std::condition_variable` set to the nearest `readyAt` time point.

### Rationale
- Zero CPU spinning or busy-waiting.
- No dedicated background timer threads required.
- New urgent submissions immediately wake waiting workers via `notify_one()`, overriding the retry delay for other jobs.

---

## 5. Immutable Snapshots & Observer Decoupling

### Context
Persistence and metrics reporting require inspecting job statuses and properties while worker threads might be executing them.

### Decision
`Job` provides a `snapshot()` method that returns a copy of `JobSnapshot`. In addition, `JobManager` injects a `JobStateObserver` into each job to notify listeners of state changes.

### Rationale
- Handlers and storage writers do not hold locks on `Job::statusMutex_` during disk I/O.
- Persistence is completely decoupled: `Job` does not depend on `JobStorage` or any file system headers.

---

## 6. Monotonic `steady_clock` for Metrics

### Context
System clocks (`std::chrono::system_clock`) can jump backwards or forwards due to NTP time synchronization, daylight saving adjustments, or manual user changes.

### Decision
All execution timings, retry delays, and throughput statistics use `std::chrono::steady_clock`.

### Rationale
- Guaranteed monotonic progression. Durations and execution latencies are never negative.
- Preserves absolute precision down to nanoseconds.

---

## 7. Append-Only Text Journal with Incomplete Tail Repair

### Context
Application crashes during file writes can corrupt database files and journal records.

### Decision
`FileJobStorage` uses a simple delimited, append-only journal format. Upon opening the file, `repairIncompleteTailLocked()` inspects the file end and trims any truncated trailing characters back to the last valid newline.

### Rationale
- Simple, transparent, and inspectable with standard text utilities.
- Resilient to unexpected power loss or process kill signals without complex WAL transaction logs.
- Fast append operations during normal runtime.
