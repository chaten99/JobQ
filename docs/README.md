# JobQ Documentation Index

Welcome to the **JobQ** documentation suite. This index provides links to all architectural guides, API manuals, CLI references, and technical specifications.

---

## 1. Getting Started
- **[Installation & Setup Guide](installation.md):** Complete guide to setting up the MSYS2 MinGW UCRT64 C++20 toolchain, configuring CMake, compiling executables, and running tests.
- **[Project Structure Reference](project-structure.md):** Breakdown of repository directories, header layouts, source organization, test suites, and build scripts.

---

## 2. Architecture & Design
- **[System Architecture](architecture.md):** High-level component interactions, Mermaid architecture diagrams, execution pipelines, and data flow.
- **[Design Decisions & Trade-Offs](design-decisions.md):** Rationale behind using C++20, in-memory sequence-based priority heaps, delayed CV retries, and immutable snapshots.

---

## 3. Core Engine Mechanics
- **[Job Lifecycle & State Transitions](job-lifecycle.md):** State machine rules (`Pending`, `Running`, `Completed`, `Failed`, `Cancelled`), attempt counting, and snapshot invariants.
- **[Queue & Worker Concurrency](queue-and-workers.md):** Thread-safe priority queues, deterministic sequence-based FIFO ordering, delayed retry min-heaps, and worker pool lifecycles.
- **[Retry System & Exponential Backoff](retry-system.md):** `RetryPolicy` configuration, exponential delay calculations, attempt tracking, and worker failure handling.
- **[Persistence & Crash Recovery](persistence-and-recovery.md):** Append-only text journals, `JobRecord` serialization, automatic incomplete tail truncation, and startup recovery reconciliation.

---

## 4. CLI & Tooling
- **[CLI Reference Guide](cli-reference.md):** Complete manual for `jobq_cli` subcommands (`submit`, `list`, `status`, `run`, `metrics`), argument flags, and output formats (formatted text and JSON).

---

## 5. Observability
- **[Metrics & Observability](metrics-and-observability.md):** In-memory counter semantics, monotonic execution duration measurements, throughput tracking, and Schema Version 1 JSON metrics export.

---

## 6. Developer Guides & API Reference
- **[C++ API Usage Guide](api-usage.md):** Code examples showing how to embed `JobQueue`, `JobExecutor`, `WorkerPool`, `JobManager`, and `JobMetrics` into modern C++20 applications.
- **[Examples & Benchmarks](examples.md):** Overview of standalone demonstration binaries, including the multi-producer concurrency stress test (`jobq_stress_test`) and in-process demo (`jobq`).

---

## 7. Quality Assurance & Project Evolution
- **[Testing Guide](testing.md):** Overview of the 19 automated CTest test suites, concurrency verification using latches, and instructions for adding new test targets.
- **[Contributing Guide](../CONTRIBUTING.md):** Developer workflow, Git branch conventions, coding standards, and PR verification checklist.
- **[Project Status & Roadmap](roadmap.md):** Detailed breakdown of implemented capabilities (Phases 1â€“8) versus upcoming future milestones (HTTP API, web dashboard, library packaging).
