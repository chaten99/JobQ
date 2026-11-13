# Project Status & Roadmap

This document outlines the current verified capabilities of JobQ, active development work, and future architectural milestones.

---

## 1. Verified & Implemented Capabilities (Phases 1-9)

The following capabilities have been fully implemented, integrated, and verified against the 27 automated CTest test suites:

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

### Reliability & Fault Tolerance (Phase 9)
- [x] 9.1 Graceful shutdown: close-and-drain `WorkerPool::stop()`, idempotent and concurrent-safe (`jobq_worker_shutdown`).
- [x] 9.2 Worker lifecycle hardening: single-use `Worker`, safe join/destroy, exception boundary, partial-startup rollback (`jobq_worker_lifecycle`).
- [x] 9.3 Queue reliability: atomic push-versus-close, exception-safe retry promotion, concurrency tests (`jobq_queue_reliability`).
- [x] 9.4 Persistence and recovery hardening: record-level validation, validate-first recovery, next-ID rules (`jobq_recovery_hardening`).
- [x] 9.5 Fault injection and resilience tests (`jobq_fault_injection`).
- [x] 9.6 Reliability documentation: [Reliability Guide](reliability.md) and companion guides.

Known reliability limits (at-least-once execution, no `fsync`, no cross-process locking, no handler timeouts) are listed in the [Reliability Guide](reliability.md#5-known-limitations).

### Test Automation
- [x] 27 automated test suites integrated with CTest.
- [x] Concurrency stress tests and deterministic latch-based synchronization proofs.

---

## 2. In Progress

- No phase is currently in progress.

---

## 3. Future Roadmap & Planned Milestones

> [!IMPORTANT]
> The features listed below are **planned design milestones**. They are not currently implemented in the codebase.
> Current JobQ persistence and queueing operate on a single host within a single process; they do not automatically provide multi-process distributed coordination.

### Phase 10: Library Packaging & Public C++ SDK
- [ ] CMake target export (`find_package(JobQ)`) and header installation.
- [ ] Reusable shared/static library targets (`libjobq`).
- [ ] Ergonomic single-include top-level header (`jobq/jobq.h`).
- [ ] C++20 module interface support (`jobq.ixx`).

### Phase 11: Long-Running Daemon & Journal Compaction
- [ ] Background daemon mode with signal handling (`SIGINT`, `SIGTERM`).
- [ ] Journal compaction / snapshot vacuuming to prevent unbounded growth of append-only log files.
- [ ] Dynamic worker pool scaling (resizing active workers at runtime).

### Phase 12: HTTP API & Web Dashboard
- [ ] Embedded HTTP REST API server for remote job submission and status inspection.
- [ ] WebSocket streaming of live runtime metrics and worker states.
- [ ] Modern web dashboard (React / Vite) for visualizing queue throughput, worker health, and failure logs.

### Phase 13: Distributed Queue Backend
- [ ] Distributed storage and locking adapter (e.g., Redis or Raft-backed cluster).
- [ ] Multiple independent worker processes consuming from a shared distributed queue.
- [ ] Heartbeat mechanisms and worker lease timeouts for dead worker detection.
