# Testing Guide & Quality Assurance

JobQ maintains high code quality and concurrency reliability through an extensive automated test suite integrated with CMake's **CTest**.

---

## 1. Test Architecture

JobQ uses a modular testing philosophy:
- **No External Test Framework Dependencies:** Tests are implemented as lightweight C++ executables returning `0` on success and non-zero on failure.
- **Independent Test Executables:** Each subsystem has dedicated test binaries ensuring isolation and fast parallel execution.
- **Deterministic Concurrency Verification:** Concurrency tests use `std::latch` and atomic barriers rather than fragile arbitrary sleep timers.

---

## 2. Test Suite Catalog (27 Registered Tests)

| # | CTest Name | Source File | Description |
|---|---|---|---|
| 1 | `jobq_manager_lifecycle` | `tests/job_manager_test.cpp` | In-memory job creation, registry lookups, status transitions, and atomic ID allocation. |
| 2 | `jobq_priority_scheduling` | `tests/job_priority_test.cpp` | Priority queue ordering (`High` > `Normal` > `Low`) and sequence-based FIFO tie-breaking. |
| 3 | `jobq_retry_policy` | `tests/job_retry_policy_test.cpp` | Retry eligibility and exponential backoff delay calculations. |
| 4 | `jobq_retry_execution` | `tests/job_retry_execution_test.cpp` | Worker retry loop, status transitions, and final failure when retries are exhausted. |
| 5 | `jobq_retry_backoff` | `tests/job_retry_backoff_test.cpp` | Timing verification of delayed retry promotion from min-heap to active queue. |
| 6 | `jobq_storage_record` | `tests/job_record_test.cpp` | Serialization and deserialization of delimited `JobRecord` strings. |
| 7 | `jobq_file_storage` | `tests/job_file_storage_test.cpp` | Disk journal persistence, record reloads, and corrupted tail truncation repair. |
| 8 | `jobq_manager_persistence` | `tests/job_manager_persistence_test.cpp` | Automatic snapshot persistence triggered by `JobStateObserver`. |
| 9 | `jobq_crash_recovery` | `tests/job_recovery_test.cpp` | Recovery of pending and interrupted running jobs upon restart. |
| 10 | `jobq_persistence_recovery_regression` | `tests/job_persistence_recovery_regression_test.cpp` | Edge-case regression test verifying duplicate ID reconciliation. |
| 11 | `jobq_cli_foundation` | `tests/cli_foundation_test.cpp` | CLI argument parser, option flag extraction, and invalid syntax rejection. |
| 12 | `jobq_cli_submit` | `tests/cli_submit_test.cpp` | CLI `submit` command integration and storage validation. |
| 13 | `jobq_cli_inspection` | `tests/cli_inspection_test.cpp` | CLI `list` and `status` commands. |
| 14 | `jobq_cli_run` | `tests/cli_run_test.cpp` | CLI `run` command worker pool execution from journal. |
| 15 | `jobq_cli_workflow` | `tests/cli_workflow_test.cpp` | End-to-end multi-command workflow test exercising submit, inspect, and run cycles. |
| 16 | `jobq_metrics_model` | `tests/job_metrics_test.cpp` | `JobMetrics` unit tests: counter increments, duration metrics, and snapshot invariants. |
| 17 | `jobq_cli_metrics` | `tests/cli_metrics_test.cpp` | Metrics formatting and reporting in `run` and standalone `metrics`. |
| 18 | `jobq_metrics_integration` | `tests/job_metrics_integration_test.cpp` | Multi-threaded concurrency test with multiple workers and producers under contention. |
| 19 | `jobq_metrics_json` | `tests/job_metrics_json_test.cpp` | Schema v1 JSON export, nanosecond duration precision, and CLI `--format json`. |
| 20 | `jobq_worker_shutdown` | `tests/worker_shutdown_test.cpp` | Graceful shutdown: active jobs, queue draining, rejected submissions, repeated stop, stop before start, retry during shutdown. |
| 21 | `jobq_worker_lifecycle` | `tests/worker_lifecycle_test.cpp` | `Worker` / `WorkerPool` lifecycle hardening: repeated start/join/stop, self-join, destructor safety. |
| 22 | `jobq_queue_reliability` | `tests/job_queue_reliability_test.cpp` | Concurrent producers/consumers, push versus close, ordering, delayed retries. |
| 23 | `jobq_recovery_hardening` | `tests/job_recovery_hardening_test.cpp` | Record validation, recovery of every job state, duplicate IDs, next-ID rules, storage failures. |
| 24 | `jobq_fault_injection` | `tests/fault_injection_test.cpp` | Injected handler, storage, queue and worker failures; corruption; concurrent failure stress; metrics invariants. |
| 25 | `jobq_latency_statistics` | `tests/job_latency_statistics_test.cpp` | Latency statistics (nearest-rank percentiles, sums, merging), queue-ready timestamps, and per-attempt observer accounting through a real `WorkerPool`. |
| 26 | `jobq_worker_scaling_statistics` | `tests/worker_scaling_statistics_test.cpp` | Worker-count list parsing and validation, repeated-run summaries, speedup, efficiency, throughput change and baseline handling (pure arithmetic, no timing). |
| 27 | `jobq_queue_stress` | `tests/job_queue_stress_test.cpp` | Bounded stress integration test (concurrent producers and consumers, sustained submission, priorities, drain, metrics consistency, repeated runs, concurrent first use of a fresh `JobManager`) built on `benchmarks/StressRunner.h`. |

---

## 3. Running Tests

### Run All Tests
Execute all 27 test suites with failure output enabled:

```powershell
ctest --test-dir build --output-on-failure
```

### Run Tests in Parallel
To speed up local test runs using multiple threads:

```powershell
ctest --test-dir build -j 4 --output-on-failure
```

### Run a Specific Test
Filter tests by regex or exact name:

```powershell
# Run only JSON metrics test
ctest --test-dir build -R jobq_metrics_json --output-on-failure

# Run all CLI-related tests
ctest --test-dir build -R "jobq_cli_*" --output-on-failure
```

---

## 4. Phase 9 Reliability Testing

Phase 9 added five test executables. They link `${JOBQ_CORE_SOURCES}` like every other target; `jobq_fault_injection_test` additionally has `tests/` on its include path for the shared helpers in `tests/helpers/`.

| Area | CTest name (executable) | Source | Verifies |
|---|---|---|---|
| Graceful shutdown | `jobq_worker_shutdown` (`jobq_worker_shutdown_test`) | `tests/worker_shutdown_test.cpp` | Shutdown waits for an active handler; queued jobs drain; submissions after shutdown are rejected and not counted; repeated and concurrent `stop()`; stop before start; retry during shutdown; persistence compatibility. |
| Worker lifecycle | `jobq_worker_lifecycle` (`jobq_worker_lifecycle_test`) | `tests/worker_lifecycle_test.cpp` | Worker/pool start and join rules, self-join and self-stop rejection, destructor safety, handler exception survival, start after stop. |
| Queue concurrency, priority, delayed retries | `jobq_queue_reliability` (`jobq_queue_reliability_test`) | `tests/job_queue_reliability_test.cpp` | Producers/consumers, push versus close, close wake-ups, priority and FIFO, retry timing and promotion, `size()`/`empty()`. |
| Persistence hardening and recovery | `jobq_recovery_hardening` (`jobq_recovery_hardening_test`) | `tests/job_recovery_hardening_test.cpp` | Record validation, recovery of Pending/Running/Failed/Completed/Cancelled, duplicates, invalid records, next-ID rules, read/write failures, closed-queue recovery, repeated restarts. |
| Fault injection and metrics under faults | `jobq_fault_injection` (`jobq_fault_injection_test`) | `tests/fault_injection_test.cpp` | Handler, storage, queue, corruption and worker-lifecycle faults; 400-job concurrent failure stress; metrics invariants after every scenario. |

The pre-existing `jobq_priority_scheduling`, `jobq_retry_*`, `jobq_file_storage`, `jobq_crash_recovery` and `jobq_persistence_recovery_regression` tests also cover parts of this area and continue to run unchanged.

### Fault-injection helpers (`tests/helpers/`)

| File | Purpose |
|---|---|
| `FaultInjectingJobStorage.h` | In-memory `JobStorage` with switchable read failure and predicate-based save failure; records saves. |
| `TemporaryDirectory.h` | Unique temp directory for journal files, removed on destruction. |
| `TestSupport.h` | Bounded-wait `Gate` and `Counter`, `throwsWith`, `waitUntil`, `metricsConsistent`. |

### Running the Phase 9 tests

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

# Only the reliability tests
ctest --test-dir build -R "jobq_worker_shutdown|jobq_worker_lifecycle|jobq_queue_reliability|jobq_recovery_hardening|jobq_fault_injection" --output-on-failure

# One test, repeated to look for intermittent failures
ctest --test-dir build -R jobq_fault_injection --repeat until-fail:20 --timeout 120
```

Each executable can also be run directly, for example `.\build\jobq_fault_injection_test.exe`; it prints one `[PASS]`/`[FAIL]` line per check and exits non-zero on failure.

### Special conditions

- The tests use bounded waits (10 s) and condition variables or promises for synchronization. Delayed-retry tests use real 100-200 ms delays because they test elapsed time.
- Journal tests use unique temp directories and remove them afterwards; no project storage file is touched.
- Run `ctest` from the shell that built the project. On Windows, a shell without the toolchain on `PATH` makes test binaries fail with `0xc0000139`.
- `jobq_fault_injection` takes a few seconds (the stress scenario runs 400-job workloads on 6 workers).

### Not deterministically tested

- `std::thread` creation failure and the `WorkerPool::start()` rollback (needs a production thread-factory hook; reviewed in code only).
- Allocation failure inside `JobQueue` promotion.
- An unexpected exception escaping `Worker::processJob` or a throwing `waitAndPop()`.
- A failing `Cancelled` save after a rejected submission.
- Real process kills and OS-level write loss; crash recovery is tested by seeding or hand-writing journal records.

See [Fault Tolerance](fault-tolerance.md) for the matrix of scenarios and [Reliability](reliability.md) for the guarantees these tests back.

---

## 5. Adding a New Test Target

To add a new test to the project:

1. Create your test file under `tests/` (e.g. `tests/my_feature_test.cpp`).
2. Include the relevant core headers and implement your test logic in `main()`:
   ```cpp
   #include <cassert>
   #include <iostream>
   #include "queue/JobQueue.h"

   int main() {
       jobq::JobQueue queue;
       assert(queue.empty());
       std::cout << "[PASS] My feature test passed!\n";
       return 0; // Return 0 for PASS, non-zero for FAIL
   }
   ```
3. Register the executable and test in [`CMakeLists.txt`](../CMakeLists.txt) within the `if(BUILD_TESTING)` block:
   ```cmake
   add_executable(my_feature_test
       tests/my_feature_test.cpp
       ${JOBQ_CORE_SOURCES}
   )

   target_include_directories(my_feature_test PRIVATE
       ${CMAKE_CURRENT_SOURCE_DIR}/include
   )

   target_link_libraries(my_feature_test PRIVATE Threads::Threads)

   add_test(
       NAME my_feature
       COMMAND my_feature_test
   )
   ```
4. Rebuild and run:
   ```powershell
   cmake --build build
   ctest --test-dir build -R my_feature --output-on-failure
   ```
