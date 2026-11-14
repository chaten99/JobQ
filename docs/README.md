# JobQ Documentation Index

Welcome to the **JobQ** documentation suite. This index links to the architectural guides, API manuals, reliability guides, CLI reference and technical specifications.

---

## 1. Getting Started
- **[Installation & Setup Guide](installation.md):** Complete guide to setting up the MSYS2 MinGW UCRT64 C++20 toolchain, configuring CMake, compiling executables, and running tests.
- **[Project Structure Reference](project-structure.md):** Breakdown of repository directories, header layouts, source organization, test suites, and build scripts.
- **[C++ API Usage Guide](api-usage.md):** Code examples showing how to embed `JobQueue`, `JobExecutor`, `WorkerPool`, `JobManager`, and `JobMetrics` into modern C++20 applications.
- **[Examples & Benchmarks](examples.md):** Standalone demonstration binaries, including the multi-producer stress test (`jobq_stress_test`) and the in-process demo (`jobq`).

---

## 2. Architecture
- **[System Architecture](architecture.md):** High-level component interactions, Mermaid diagrams, execution pipelines, and data flow.
- **[Design Decisions & Trade-Offs](design-decisions.md):** Rationale behind C++20, in-memory sequence-based priority heaps, delayed condition-variable retries, and immutable snapshots.

---

## 3. Core Concepts
- **[Job Lifecycle & State Transitions](job-lifecycle.md):** State machine rules (`Pending`, `Running`, `Completed`, `Failed`, `Cancelled`), attempt counting, and snapshot invariants.
- **[Retry System & Exponential Backoff](retry-system.md):** `RetryPolicy` configuration, delay calculation, attempt tracking, and failure handling.

---

## 4. Queue & Workers
- **[Queue & Worker Concurrency](queue-and-workers.md):** Thread-safe priority queue, sequence-based FIFO ordering, delayed retry min-heap, worker loop and pool overview.
- **[Queue Reliability](queue-reliability.md):** Synchronization model, push/close semantics, priority and FIFO guarantees, retry promotion and notifications.
- **[Worker Lifecycle](worker-lifecycle.md):** `Worker` and `WorkerPool` lifecycles, exception boundaries, startup rollback and the concurrency contract.

---

## 5. Persistence
- **[Persistence & Crash Recovery](persistence-and-recovery.md):** Append-only journal, record format and validation, startup recovery, operational behaviour after a restart, and limitations.

---

## 6. Reliability
- **[Reliability Guide](reliability.md):** Entry point for Phase 9: components, guarantees table with test evidence, and known limitations.
- **[Graceful Shutdown](graceful-shutdown.md):** The close-and-drain policy, per-scenario behaviour and shutdown flow.
- **[Fault Tolerance](fault-tolerance.md):** Verified failure scenarios, the fault-handling matrix, and known gaps.

---

## 7. Metrics
- **[Metrics & Observability](metrics-and-observability.md):** In-memory counter semantics, monotonic execution durations, throughput, and the Schema Version 1 JSON export.

---

## 8. CLI
- **[CLI Reference Guide](cli-reference.md):** `jobq_cli` subcommands (`submit`, `list`, `status`, `run`, `metrics`), flags, and output formats.

---

## 9. Testing
- **[Benchmarking Guide](benchmarking.md):** The configurable throughput benchmark (`jobq_throughput_benchmark`): scenarios, options, methodology and limitations. Includes latency, worker scaling, queue stress and contention analysis.
- **[Testing Guide](testing.md):** The 28 CTest suites, the Phase 9 reliability tests and helpers, how to run them, and how to add a new test target.
- **[Contributing Guide](../CONTRIBUTING.md):** Developer workflow, Git conventions, coding standards, and the PR checklist.

---

## 10. Troubleshooting
- **[Troubleshooting Guide](troubleshooting.md):** Diagnosing build, runtime and persistence problems without risking job data.

---

## 11. Roadmap
- **[Project Status & Roadmap](roadmap.md):** Implemented capabilities (Phases 1-9) and planned milestones.
