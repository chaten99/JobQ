# Examples & Demonstrations

JobQ includes reference demonstrations and benchmark harnesses to verify system behavior under load.

---

## 1. Concurrency Stress Test (`jobq_stress_test`)

### Overview
- **File Location:** [`examples/stress_test.cpp`](../examples/stress_test.cpp)
- **Target Name:** `jobq_stress_test`
- **Purpose:** Verifies thread safety, priority queue integrity, and worker contention under heavy concurrent workloads.

### What It Demonstrates
- Multi-producer, multi-consumer concurrency: Multiple producer threads simultaneously submit hundreds of jobs with varying priorities (`High`, `Normal`, `Low`).
- Concurrent worker pool: Multiple worker threads dequeue and process jobs simultaneously without race conditions or deadlocks.
- Ordering verification: High-priority jobs are prioritized under contention.
- Clean shutdown: Coordinated queue closure and thread joining after queue drainage.

### How to Build
```powershell
cmake --build build --target jobq_stress_test
```

### How to Run
```powershell
.\build\jobq_stress_test.exe
```

### Sample Output
```text
=== JobQ Concurrency Stress Test ===
Starting 4 worker threads...
Starting 4 producer threads submitting 500 jobs each...
Producers finished submitting 2000 jobs.
Waiting for workers to drain queue...
All jobs executed successfully.
Queue size: 0
Worker pool stopped cleanly.
=== Test Completed Successfully ===
```

---

## 2. In-Process Runtime Demo (`jobq`)

### Overview
- **File Location:** [`src/main.cpp`](../src/main.cpp)
- **Target Name:** `jobq`
- **Purpose:** Demonstrates full integration of `JobManager`, `JobExecutor`, `WorkerPool`, exponential backoff retries, and `JobMetrics` reporting.

### What It Demonstrates
- Creating handlers for different job types (e.g. `EMAIL`, `REPORT`).
- Simulating transient failure with a retry policy (demonstrating backoff delays).
- Collecting execution timings with `JobMetrics`.
- Outputting formatted metrics tables at shutdown.

### How to Build
```powershell
cmake --build build --target jobq
```

### How to Run
```powershell
.\build\jobq.exe
```

### Sample Output
```text
=== JobQ In-Process Demo ===
[Worker 1] picked Job 1 (EMAIL, priority HIGH, attempt 1)
[Handler] Sending email to user@example.com
[Worker 1] Job 1 completed after 1 attempt(s)
[Worker 2] picked Job 2 (REPORT, priority NORMAL, attempt 1)
[Handler] Simulating network glitch for REPORT...
[Worker 2] Handler failed, scheduling retry in 100ms...
[Worker 1] picked Job 2 (REPORT, priority NORMAL, attempt 2)
[Handler] Generating report...
[Worker 1] Job 2 completed after 2 attempt(s)

========== JobQ Runtime Metrics ==========

Job Statistics

  Submitted Jobs:          2
  Started Executions:      3
  Completed Executions:    2
  Failed Executions:       1
  Scheduled Retries:       1
  Finished Executions:     3

Performance Statistics

  Total Execution Time:    25.40 ms
  Average Execution Time:  8.47 ms
  Elapsed Time:            152.18 ms
  Throughput:              13.14 jobs/sec

==========================================
```
