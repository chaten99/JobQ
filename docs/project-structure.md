# JobQ Project Structure

This document provides a comprehensive guide to the directory layout, source organization, build configuration, and file responsibilities within the JobQ codebase.

---

## 1. Directory Tree

```text
JobQ/
â”œâ”€â”€ CMakeLists.txt              # Primary CMake build configuration and test registration
â”œâ”€â”€ README.md                   # Project overview, quickstart, and feature highlights
â”œâ”€â”€ CONTRIBUTING.md             # Developer workflow, coding standards, and PR guidelines
â”œâ”€â”€ docs/                       # Comprehensive documentation suite
â”‚   â”œâ”€â”€ README.md               # Documentation index and navigation map
â”‚   â”œâ”€â”€ installation.md         # Environment setup and build guide
â”‚   â”œâ”€â”€ architecture.md         # System design, data flow, and concurrency model
â”‚   â”œâ”€â”€ project-structure.md    # Source tree overview (this document)
â”‚   â”œâ”€â”€ job-lifecycle.md        # State transitions, invariants, and snapshot rules
â”‚   â”œâ”€â”€ queue-and-workers.md    # Priority queue, FIFO ordering, and worker pool
â”‚   â”œâ”€â”€ retry-system.md         # Exponential backoff retry policies and scheduling
â”‚   â”œâ”€â”€ persistence-and-recovery.md # Append journal format and crash recovery
â”‚   â”œâ”€â”€ cli-reference.md        # CLI syntax, subcommands, arguments, and exit codes
â”‚   â”œâ”€â”€ metrics-and-observability.md # Runtime counters, performance stats, and JSON export
â”‚   â”œâ”€â”€ api-usage.md            # C++20 code examples and developer guide
â”‚   â”œâ”€â”€ examples.md             # Guide to standalone examples and benchmarks
â”‚   â”œâ”€â”€ testing.md              # Test architecture, CTest suites, and test authoring
â”‚   â”œâ”€â”€ design-decisions.md     # Architectural rationale and trade-offs
â”‚   â””â”€â”€ roadmap.md              # Implemented features vs planned milestones
â”œâ”€â”€ include/                    # Public C++20 interface headers
â”‚   â”œâ”€â”€ cli/                    # CLI argument parser and command handlers
â”‚   â”œâ”€â”€ executor/               # Job handler registry and executor
â”‚   â”œâ”€â”€ job/                    # Job model, statuses, priorities, and retry policies
â”‚   â”œâ”€â”€ manager/                # JobManager, registry, and observer integration
â”‚   â”œâ”€â”€ metrics/                # JobMetrics, snapshots, and JSON serialization
â”‚   â”œâ”€â”€ queue/                  # JobQueue, sequence ordering, and delayed retries
â”‚   â”œâ”€â”€ storage/                # JobStorage interface, FileJobStorage, and JobRecord
â”‚   â””â”€â”€ worker/                 # Worker thread and WorkerPool management
â”œâ”€â”€ src/                        # C++20 implementation files
â”‚   â”œâ”€â”€ main.cpp                # Standalone demo application entry point
â”‚   â”œâ”€â”€ cli/                    # CLI implementation files and CLI main
â”‚   â”œâ”€â”€ executor/               # JobExecutor implementation
â”‚   â”œâ”€â”€ job/                    # Job model implementation
â”‚   â”œâ”€â”€ manager/                # JobManager implementation
â”‚   â”œâ”€â”€ metrics/                # JobMetrics and JobMetricsJson implementation
â”‚   â”œâ”€â”€ queue/                  # JobQueue implementation
â”‚   â”œâ”€â”€ storage/                # FileJobStorage and JobRecord serialization
â”‚   â””â”€â”€ worker/                 # Worker and WorkerPool implementations
â”œâ”€â”€ tests/                      # Automated CTest test suites (27 test drivers)
â””â”€â”€ examples/                   # Stress test and concurrency benchmark programs
```

---

## 2. Header Directories (`include/`)

All headers reside under `include/` and are organized modularly:

| File | Purpose |
|---|---|
| `include/job/Job.h` | Defines `Job`, `JobStatus`, `JobPriority`, `RetryPolicy`, `JobSnapshot`, and `JobStateObserver`. |
| `include/queue/JobQueue.h` | Defines `JobQueue` thread-safe priority queue with FIFO sequence numbers and delayed retry min-heap. |
| `include/executor/JobExecutor.h` | Defines `JobExecutor` with handler registration and execution dispatch. |
| `include/worker/Worker.h` | Defines individual `Worker` thread class consuming jobs from `JobQueue`. |
| `include/worker/WorkerPool.h` | Defines `WorkerPool` managing worker lifecycles and coordinated shutdown. |
| `include/manager/JobManager.h` | Defines `JobManager` coordinating job creation, storage persistence, and recovery. |
| `include/storage/JobStorage.h` | Abstract storage interface defining `save` and `loadAll`. |
| `include/storage/JobRecord.h` | Serializable POD record structure with `serializeJobRecord` and `deserializeJobRecord`. |
| `include/storage/FileJobStorage.h` | Journal file storage implementation with automatic corrupted-tail repair. |
| `include/metrics/JobMetrics.h` | Thread-safe `JobMetrics` counter and timing model, and `JobMetricsSnapshot`. |
| `include/metrics/JobMetricsJson.h` | Structured JSON serialization (`toJson(const JobMetricsSnapshot&)`). |
| `include/cli/CommandLine.h` | Command-line argument parser, options extractor, and usage text provider. |
| `include/cli/CommandHandlers.h` | Subcommand executors (`executeSubmitCommand`, `executeListCommand`, `executeStatusCommand`, `executeRunCommand`, `executeMetricsCommand`). |

---

## 3. Source Directories (`src/`)

Source implementations directly match their respective header headers:

- **`src/main.cpp`**: Standalone in-process demo creating a `JobQueue`, `JobManager`, registering handlers in `JobExecutor`, starting `WorkerPool`, submitting jobs with retries, and printing runtime metrics.
- **`src/cli/main.cpp`**: CLI executable entry point (`jobq_cli`).
- **`src/cli/CommandLine.cpp`**: CLI argument parsing logic and validation.
- **`src/cli/CommandHandlers.cpp`**: Handlers for all CLI operations.
- **`src/executor/JobExecutor.cpp`**: Handler map management and dispatch.
- **`src/job/Job.cpp`**: State machine validation, retry calculations, and observer notifications.
- **`src/manager/JobManager.cpp`**: Monotonic ID generation, in-memory indexing, and recovery.
- **`src/metrics/JobMetrics.cpp`**: Mutex-synchronized counter increments and performance statistics.
- **`src/metrics/JobMetricsJson.cpp`**: Schema v1 JSON export.
- **`src/queue/JobQueue.cpp`**: Priority scheduling, sequence tracking, and timed CV waits.
- **`src/storage/FileJobStorage.cpp`**: File I/O journal append and recovery.
- **`src/storage/JobRecord.cpp`**: Delimited string format serialization and parsing.
- **`src/worker/Worker.cpp`**: Worker thread loop, exception handling, and retry scheduling.
- **`src/worker/WorkerPool.cpp`**: Worker creation and lifecycle synchronization.

---

## 4. Test Directory (`tests/`)

JobQ contains 27 standalone test drivers, each compiled into its own executable and registered with CMake `add_test()`:

| Test Executable | CTest Name | Focus Area |
|---|---|---|
| `jobq_manager_test` | `jobq_manager_lifecycle` | In-memory `JobManager` submission, status queries, and ID generation. |
| `jobq_priority_test` | `jobq_priority_scheduling` | Priority queue ordering (High > Normal > Low) and FIFO tie-breaking. |
| `jobq_retry_policy_test` | `jobq_retry_policy` | Retry eligibility and exponential backoff calculations. |
| `jobq_retry_execution_test` | `jobq_retry_execution` | Worker retry loop, status transitions, and final failure states. |
| `jobq_retry_backoff_test` | `jobq_retry_backoff` | Timing verification of delayed retry queues. |
| `jobq_storage_record_test` | `jobq_storage_record` | Serialization and deserialization of `JobRecord`. |
| `jobq_file_storage_test` | `jobq_file_storage` | File journal persistence, reloads, and tail truncation recovery. |
| `jobq_manager_persistence_test` | `jobq_manager_persistence` | `JobManager` persistence integration and state observers. |
| `jobq_recovery_test` | `jobq_crash_recovery` | Crash recovery of interrupted running jobs and pending jobs. |
| `jobq_persistence_recovery_regression_test` | `jobq_persistence_recovery_regression` | Edge-case regression test for persistence reload. |
| `jobq_cli_test` | `jobq_cli_foundation` | CLI argument parsing, flags, and option validation. |
| `jobq_cli_submit_test` | `jobq_cli_submit` | CLI `submit` command integration. |
| `jobq_cli_inspection_test` | `jobq_cli_inspection` | CLI `list` and `status` commands. |
| `jobq_cli_run_test` | `jobq_cli_run` | CLI `run` command worker execution. |
| `jobq_cli_workflow_test` | `jobq_cli_workflow` | End-to-end multi-command CLI workflow simulation. |
| `jobq_metrics_test` | `jobq_metrics_model` | `JobMetrics` unit tests, counter increments, and duration stats. |
| `jobq_cli_metrics_test` | `jobq_cli_metrics` | CLI metrics reporting in `run` and standalone `metrics`. |
| `jobq_metrics_integration_test` | `jobq_metrics_integration` | High-concurrency integration test with `std::latch` and multi-worker contention. |
| `jobq_metrics_json_test` | `jobq_metrics_json` | JSON schema validation, nanosecond precision, and CLI `--format json`. |
| `jobq_worker_shutdown_test` | `jobq_worker_shutdown` | Graceful shutdown and queue draining. |
| `jobq_worker_lifecycle_test` | `jobq_worker_lifecycle` | `Worker` and `WorkerPool` lifecycle hardening. |
| `jobq_queue_reliability_test` | `jobq_queue_reliability` | Concurrent queue behaviour, closure and delayed retries. |
| `jobq_recovery_hardening_test` | `jobq_recovery_hardening` | Record validation and recovery of every job state. |
| `jobq_fault_injection_test` | `jobq_fault_injection` | Fault injection and resilience (helpers in `tests/helpers/`). |
| `jobq_latency_statistics_test` | `jobq_latency_statistics` | Latency statistics, queue-ready timestamps and execution observer accounting. |
| `jobq_worker_scaling_statistics_test` | `jobq_worker_scaling_statistics` | Worker-scaling calculations and worker-count parsing (uses `benchmarks/ScalingAnalysis.h`). |
| `jobq_queue_stress_test` | `jobq_queue_stress` | Bounded queue stress test (uses `benchmarks/StressRunner.h`). |

---

## Benchmarks Directory (`benchmarks/`)

- **`benchmarks/throughput_benchmark.cpp`**: configurable throughput benchmark (CMake target `jobq_throughput_benchmark`, not run by CTest). See [Benchmarking](benchmarking.md).
- **`benchmarks/ScalingAnalysis.h`**: header-only worker-count parsing, run summaries and speedup/efficiency calculations shared by the benchmark and its unit test.
- **`benchmarks/queue_stress_benchmark.cpp`**: configurable queue stress benchmark (CMake target `jobq_queue_stress_benchmark`, not run by CTest).
- **`benchmarks/StressRunner.h`**: header-only stress run implementation shared by the stress benchmark and `jobq_queue_stress`.
- **`benchmarks/BenchmarkCommon.h`**: workload handlers, console silencing, formatting and the per-worker latency collector shared by the benchmarks.

---

## 5. Examples Directory (`examples/`)

- **`examples/stress_test.cpp`**: Multi-threaded stress test demonstrating concurrent job submission across multiple producer threads while a worker pool executes jobs under contention.
