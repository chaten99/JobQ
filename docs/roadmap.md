# Project Status & Roadmap

This document outlines the current verified capabilities of JobQ, active development work, and future architectural milestones.

---

## 1. Verified & Implemented Capabilities (Phases 1â€“8)

The following capabilities have been fully implemented, integrated, and verified against the 19 automated CTest test suites:

### Core Task Model & State Machine
- [x] Strongly-typed `Job` entity with atomic ID allocation.
- [x] Defined lifecycle states: `Pending`, `Running`, `Completed`, `Failed`, `Cancelled`.
- [x] Invariant-enforced state transitions with thread-safe mutex protection.
- [x] Attempt counter tracking per job.
- [x] Decoupled `JobSnapshot` structures and `JobStateObserver` callbacks.

### Priority Scheduling & Worker Execution
- [x] Thread-safe `JobQueue` supporting multi-producer, multi-consumer access.
- [x] Priority tiers (`High`, `Normal`, `Low`) with deterministic FIFO tie-breaking using sequence numbers.
- [x] `JobExecutor` typed handler registration mapping string identifiers to callable handlers.
- [x] `Worker` threads with exception isolation (handlers throwing exceptions do not terminate worker threads).
- [x] `WorkerPool` lifecycle management with coordinated graceful shutdown and thread joining.

### Exponential Backoff Retry Subsystem
- [x] Configurable `RetryPolicy` (maximum retries, initial delay, multiplier, max delay clamp).
- [x] Delayed retry min-heap in `JobQueue` with timed condition variable waiting.
- [x] Automatic retry eligibility checks and execution delay calculations.
- [x] Retention of attempt counts across crashes.

### Persistence & Crash Recovery
- [x] Delimited text journal serialization via `JobRecord`.
- [x] `FileJobStorage` append-only disk persistence.
- [x] Automatic repair and truncation of incomplete file tails caused by power loss or hard kills.
- [x] Startup recovery: Last-write-wins reconciliation, re-enqueuing of pending and interrupted running jobs.

### Command-Line Interface (`jobq_cli`)
- [x] Subcommands: `submit`, `list`, `status`, `run`, `metrics`.
- [x] Configurable worker count and journal storage paths.
- [x] Exit codes: `0` for success, `1` for errors.

### Metrics & Observability
- [x] Thread-safe `JobMetrics` counter and timing collector.
- [x] Monotonic duration measurements using `std::chrono::steady_clock`.
- [x] Formatted human-readable console tables.
- [x] Standard-compliant JSON metrics export adhering to Schema Version 1 with nanosecond duration precision.
- [x] CLI `--format text` and `--format json` output options.

### Test Automation
- [x] 19 automated test suites integrated with CTest.
- [x] Concurrency stress tests and deterministic latch-based synchronization proofs.

---

## 2. In Progress

- **Phase 8, Commit 8.6:** Complete project documentation, architecture diagrams, and developer guide.

---

## 3. Future Roadmap & Planned Milestones

> [!IMPORTANT]
> The features listed below are **planned design milestones**. They are not currently implemented in the codebase.
> Current JobQ persistence and queueing operate on a single host within a single process; they do not automatically provide multi-process distributed coordination.

### Phase 9: Library Packaging & Public C++ SDK
- [ ] CMake target export (`find_package(JobQ)`) and header installation.
- [ ] Reusable shared/static library targets (`libjobq`).
- [ ] Ergonomic single-include top-level header (`jobq/jobq.h`).
- [ ] C++20 module interface support (`jobq.ixx`).

### Phase 10: Long-Running Daemon & Journal Compaction
- [ ] Background daemon mode with signal handling (`SIGINT`, `SIGTERM`).
- [ ] Journal compaction / snapshot vacuuming to prevent unbounded growth of append-only log files.
- [ ] Dynamic worker pool scaling (resizing active workers at runtime).

### Phase 11: HTTP API & Web Dashboard
- [ ] Embedded HTTP REST API server for remote job submission and status inspection.
- [ ] WebSocket streaming of live runtime metrics and worker states.
- [ ] Modern web dashboard (React / Vite) for visualizing queue throughput, worker health, and failure logs.

### Phase 12: Distributed Queue Backend
- [ ] Distributed storage and locking adapter (e.g., Redis or Raft-backed cluster).
- [ ] Multiple independent worker processes consuming from a shared distributed queue.
- [ ] Heartbeat mechanisms and worker lease timeouts for dead worker detection.
