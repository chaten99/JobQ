# Throughput Benchmarking

`jobq_throughput_benchmark` measures how many jobs per second the real JobQ engine processes end to end. It is the first step of Phase 10 and establishes a **baseline**; it does not optimize anything, and it is not a CI gate (no pass/fail performance threshold exists).

---

## 1. Purpose

- Provide a reproducible way to compare worker counts, workloads and future changes on the same machine.
- Verify correctness while measuring: a run only produces a throughput number if every accepted job completed exactly once.
- Queue-wait and execution latency statistics (section 10) and worker-scaling comparisons (section 11) are built into the same tool.
- Queue stress testing, contention analysis and performance optimization are separate future work and are not part of this tool.

---

## 2. Architecture

Source: `benchmarks/throughput_benchmark.cpp` (CMake target `jobq_throughput_benchmark`). It links the production sources (`${JOBQ_CORE_SOURCES}`) unchanged; there is no fake queue or mock worker.

Each run builds a fresh runtime:

```mermaid
flowchart LR
    Silence["Silence stdout/stderr"] --> Rt["JobQueue + JobMetrics<br>JobManager + JobExecutor"]
    Rt --> Pool["WorkerPool (N workers)<br>shares the same JobMetrics"]
    Pool --> Start["pool.start()"]
    Start --> T0(["t0 = steady_clock::now()"])
    T0 --> Submit["manager.submit() x jobs"]
    Submit --> Stop["pool.stop()<br>close queue, drain, join"]
    Stop --> T1(["t1 = steady_clock::now()"])
    T1 --> Verify["metrics snapshot + verification<br>(not timed)"]
```

`JobManager` and `WorkerPool` use the in-memory configuration (no `JobStorage`), so the benchmark measures the engine, not disk I/O.

---

## 3. Scenarios

| Scenario | Job types | Work per job |
|---|---|---|
| `noop` | `NOOP` | Only a counter increment (the verification counter). Measures engine overhead: creation, submission, queue, worker hand-off, state transitions, metrics. |
| `lightweight` | `LIGHT` | 200 iterations of an integer xor-shift/multiply mix, seeded by the job ID, added to an atomic checksum. |
| `mixed` | `MIX_SMALL`, `MIX_MEDIUM`, `MIX_LARGE`, `MIX_HASH` | Deterministic mix by job ID modulo 10: 50% small (50 iterations), 30% medium (400), 10% large (1500), 10% FNV hash over 256 steps. |

All work is deterministic (no random numbers). Each handler adds its result to an atomic checksum so the compiler cannot discard it. After the run the benchmark recomputes the expected checksum single-threaded (outside the timed region) and fails the run on a mismatch.

---

## 4. Command-Line Options

```text
--jobs N          Jobs per measured run, 1..2000000          (default 10000)
--workers N       Worker threads, 1..256                     (default 4)
--scenario NAME   noop | lightweight | mixed                 (default noop)
--runs N          Measured runs, 1..100                      (default 3)
--warmup-jobs N   Jobs in the unmeasured warmup, 0..2000000  (default 1000; 0 disables)
--no-latency      Disable latency collection (throughput only)
--worker-counts L Worker-scaling mode, e.g. 1,2,4,8,16 (excludes --workers)
--help            Show usage
```

Both `--name value` and `--name=value` are accepted. Numbers must be complete, non-negative decimal integers (no sign, suffix or whitespace); anything else is rejected with an error message and exit code 2. The upper limits keep memory bounded: every submitted job stays in the `JobManager` registry for the whole run.

Exit codes: `0` all runs succeeded (or `--help`), `1` a warmup or measured run failed, `2` invalid arguments.

---

## 5. Build and Run

```powershell
cmake -S . -B build
cmake --build build
.\build\jobq_throughput_benchmark.exe --jobs 100000 --workers 4 --scenario noop --runs 5 --warmup-jobs 1000
```

Smoke tests:

```powershell
.\build\jobq_throughput_benchmark.exe --jobs 1000 --workers 2 --scenario noop --runs 2 --warmup-jobs 100
.\build\jobq_throughput_benchmark.exe --jobs 1000 --workers 2 --scenario lightweight --runs 2 --warmup-jobs 100
.\build\jobq_throughput_benchmark.exe --jobs 1000 --workers 2 --scenario mixed --runs 2 --warmup-jobs 100
```

The benchmark is **not** registered with CTest, so `ctest` never runs it. The pure calculations behind the scaling mode (worker-count parsing, run summaries, speedup and efficiency) live in `benchmarks/ScalingAnalysis.h` and are covered by the CTest target `jobq_worker_scaling_statistics`; latency statistics are covered by `jobq_latency_statistics`. Neither test measures performance.

---

## 6. Methodology

### Warmup
If `--warmup-jobs` is greater than zero, one complete run with that many jobs executes first on its own runtime (queue, metrics, manager, executor, pool). It primes code paths, caches and the allocator. Its numbers are discarded; it never touches the measured runs' counters, timing or statistics. A failed warmup fails the benchmark.

### Measured runs
Every measured run constructs a fresh runtime, so counters start at zero and no state carries over.

### Timed region
- **Starts** immediately before the first `submit()` (after the pool is started and handlers are registered).
- **Ends** after `WorkerPool::stop()` returns, which closes the queue, lets workers drain all accepted jobs and joins every worker.
- Handler registration, pool start, and all verification are excluded.

### Throughput

```text
Throughput (jobs/sec) = Completed Jobs / (t1 - t0 in seconds)
```

`Completed Jobs` is `completedExecutions` from the final `JobMetrics` snapshot. Because no retries are configured, completed executions equal unique jobs.

### Run validity
A run is **successful** only if all of these hold, otherwise it is reported as unsuccessful with an error and no throughput:

- every requested job was accepted by `submit()`;
- `submittedJobs == startedExecutions == finishedExecutions`;
- `completedExecutions == submittedJobs` and `failedExecutions == 0`;
- the handler ran exactly once per job;
- every accepted job is in the `Completed` state;
- the queue is empty and the pool is stopped;
- the workload checksum matches the single-threaded expectation.

### Repeated-run statistics
Over the successful runs: minimum, maximum, arithmetic mean and median (samples sorted; odd count: middle element; even count: mean of the two middle elements). Values are printed with two decimals.

---

## 7. Benchmark Throughput Versus JobMetrics Throughput

`JobMetricsSnapshot::throughputJobsPerSecond` divides finished executions by the time since the `JobMetrics` object was **constructed**, which includes setup before the timer starts. The benchmark result uses its own wall-clock window. The report prints the JobMetrics value on each run only for reference, labelled "Metrics Lifetime Rate", and never uses it in the summary. Likewise, "Metrics Exec Total/Average" are the summed handler execution times from `JobMetrics`, not the benchmark duration.

---

## 8. Console Logging

Workers write per-job lines to `std::cout`/`std::cerr`, which would make a large run measure terminal I/O. Production logging is unchanged. Instead, each run installs a scoped null stream buffer on `cout` and `cerr` before the workers start. Workers build their output with `std::osyncstream`, which picks up the current buffer of the stream at construction, so the redirect takes effect. The original buffers are restored by a destructor that runs only after the pool is stopped (workers joined), also on error paths. The configuration and results are printed while the real buffers are in place.

Per-job logging is therefore **suppressed during measurement**.

---

## 9. Sample Output

The structure of a report (values omitted on purpose):

```text
========== JobQ Throughput Benchmark ==========

Configuration
  Scenario / Requested Jobs / Workers / Repetitions / Warmup Jobs / Per-job logging

Run Results

Run 1
  Requested, Accepted, Submitted, Started, Finished, Completed, Failed
  Duration, Throughput, Metrics Exec Total / Average / Lifetime Rate

Performance Summary
  Successful Runs, Minimum, Maximum, Mean, Median Throughput

  Benchmark Status: PASS
```

---

## 10. Latency Measurement

Unless `--no-latency` is given, every run also collects per-attempt latency samples. These are benchmark statistics; the existing `JobMetrics` counters keep their meaning.

### Definitions and timestamp boundaries

| Metric | Start | End |
|---|---|---|
| **Queue waiting time** | The moment the job became available in the ready queue (`JobQueue` stamps `Job::queueReadyTime()` under its mutex when the job enters the ready heap) | The moment `waitAndPop()` returned to the worker, before `markRunning()` |
| **Execution duration** | Immediately after `markRunning()` succeeded | Immediately after `markCompleted()` (success) or at the catch block (failure) |
| **End-to-end processing time** | Just before `JobManager::submit()` is called (benchmark side) | End of the successful attempt |

- Queue waiting time is measured at the real queue boundary, not from the start of the benchmark or from a batch timestamp. It excludes job creation and the `markRunning()` transition.
- A retry that waits out its backoff delay is not counted as queue wait: a delayed retry is stamped with its eligibility time when it is promoted into the ready heap, and a zero-delay retry is stamped when it is pushed. (The throughput benchmarks configure no retries, so this only matters for library users of `ExecutionObserver`.)
- The execution duration is the very same value that is passed to `JobMetrics::recordExecutionCompleted/Failed`; it is measured once. It therefore includes the handler, the per-job log line (silenced here) and the `markCompleted()` lifecycle transition, but not `markRunning()`. The sum of all execution samples must equal `JobMetrics` total execution time exactly; the benchmark verifies this on every run.
- End-to-end time starts at the submit call, so it includes job creation and submission. Because no retries are configured, every successful attempt is the job's final completion.
- Each execution attempt is its own sample. Successful and failed attempts are stored separately. A failed attempt contributes a queue-wait and a failed-execution sample.

### Production hooks used

A small, general-purpose instrumentation was added to the engine: `Job::queueReadyTime()` (stamped by `JobQueue`) and an optional `ExecutionObserver` that `Worker` calls once per finished attempt. It is enabled only through a `WorkerPool` constructor overload that takes an observer; existing constructors are unchanged and behave as before.

### Statistics

- Samples are nanoseconds (`LatencySamples`); the console shows human-readable units.
- Average = sum of samples / number of samples (the sum is kept exact beyond 64 bits).
- Percentiles use the **nearest-rank** method on the sorted samples: `rank = ceil(p/100 * n)` (integer arithmetic), value = the sample at that rank. For an even number of samples **p50 is the lower middle sample**; this differs from the arithmetic median (mean of the two middle values) that the throughput summary uses for per-run throughput.
- An empty sample set has no statistics and is printed as "no samples", never as zeros.

### Collection and overhead

Each worker writes only to its own cache-line-aligned slot (indexed by worker id), so there is no lock or shared counter on the hot path. Slots are merged after the pool has been stopped and all workers joined, then validated: queue-wait samples = started executions, successful + failed execution samples = completed + failed executions, end-to-end samples = completed jobs, no negative values, and sample sums = `JobMetrics` totals.

Overhead is not free: the queue takes one extra clock read per enqueue (always on), and with latency enabled there are extra clock reads per attempt and per submission plus vector appends. For micro-jobs this is a measurable fraction of the per-job cost. Use `--no-latency` for throughput numbers with the least instrumentation, and compare runs only when they use the same setting.

### Output

Each run prints one compact line per category (`n=`, average, p50, p95, p99). After all runs a "Latency Statistics" section pools the samples of all successful measured runs (omitted above 5,000,000 retained samples) with samples, minimum, maximum, average, P50, P95 and P99 for queue waiting time, successful and failed execution duration, and end-to-end time.

---

## 11. Worker Scaling Benchmarks

`--worker-counts LIST` switches the same executable into a scaling comparison: the identical workload is run for each worker count and the results are compared.

```powershell
.\build\jobq_throughput_benchmark.exe --jobs 100000 --worker-counts 1,2,4,8,16 --scenario lightweight --runs 5 --warmup-jobs 1000
```

### Worker-count list

- Comma-separated positive integers, each 1..256, at most 32 entries. Whitespace, signs, decimals, empty entries (`1,,2`, trailing commas), overflow and duplicates are rejected with a message and exit code 2.
- The list is sorted ascending; configurations execute **sequentially** in that order, never concurrently.
- `--worker-counts` cannot be combined with `--workers` (the run is rejected, nothing is silently ignored) and cannot be given twice. Without `--worker-counts` the single-configuration behaviour described above is unchanged.

### Methodology

For each worker count: an independent warmup runtime (`--warmup-jobs`), then `--runs` measured runs, each with a fresh queue, metrics, manager, executor and pool. Jobs, job types, payloads, handlers and (absent) retry configuration are identical for every count; only the worker count changes. Timing boundaries, verification, logging suppression and latency collection are exactly those of the single-configuration benchmark. A failed warmup or run marks that configuration FAILED (it is shown, excluded from comparisons and fails the benchmark) but the remaining counts still execute.

### Calculations

All relative values use the **median throughput** of each configuration (the arithmetic median of its per-run throughputs) and the one-worker configuration as baseline.

```text
Speedup (N)            = median throughput (N workers) / median throughput (1 worker)
Scaling efficiency (N) = Speedup (N) / N * 100            (not clamped; may exceed 100%)
Throughput change (N)  = median(N) - median(1)   [jobs/sec], and as a percentage of median(1)
```

- If no successful 1-worker configuration with positive throughput exists, speedup, efficiency and change are reported as unavailable; nothing is fabricated.
- Per configuration the report shows min, max, mean and median throughput, completed jobs per run, the JobMetrics average execution time, and (pooled over its successful runs) queue-wait and execution-duration statistics with the percentiles described above.
- "Best median throughput" and "best single-run throughput" name the configuration with the highest value; the largest worker count is not assumed to be best.
- Throughput is flagged as **decreased** when a configuration's median is below the previous successful configuration's median. Scaling efficiency is flagged as **declined** when it is lower than at the previous successful configuration; this is how diminishing returns are reported (no fixed threshold is used).

### Interpreting results

Adding workers does not guarantee linear scaling, and for small jobs it can reduce throughput. The producer is a single thread and all workers share one queue mutex and condition variable, so when a job's work is only microseconds the cost of submitting, locking, waking workers and recording metrics dominates; more workers then mainly add contention and context switching. Expect results to depend on CPU cores, OS scheduling, synchronization cost, queue contention, handler workload, system load and compiler optimization settings. Queue waiting time and throughput must be read together: with one worker the queue can build a backlog (large queue wait), while many workers drain it at the cost of per-job overhead; lower queue wait does not imply higher throughput.

### Limitations specific to scaling runs

- All configurations run in one process. Run-to-run noise is considerable (single runs have differed by tens of percent) and later runs in a long-lived process have been observed to slow down on the development machine; cross-check surprising results by running each count in its own invocation (`--worker-counts 4`, a one-entry list) and by comparing medians over several runs.
- Worker counts above the number of hardware threads measure oversubscription, not parallel speedup.
- The built-in scenarios have very small per-job cost and a single producer, so they characterize engine overhead and contention rather than a CPU-bound workload that would scale well.
- Pooled latency percentiles combine all successful runs of a configuration; they are not per-run values.

---

## 12. Measurement Limitations

- Results depend on CPU model, operating system, compiler and version, **optimization flags** (the default CMake configuration here is `Debug` without `-O`), worker count, workload, system load and thermal state. Do not compare numbers across machines or builds.
- The `noop` and `lightweight` jobs are tiny, so results mostly reflect queue and synchronization overhead; they say little about real handlers.
- The timed region includes job creation and submission on the single submitting thread, so for very cheap jobs the producer can be the bottleneck.
- No persistence is used; real deployments with `FileJobStorage` will be slower.
- Every job stays registered in memory until the run ends, hence the 2,000,000-job cap.
- Short runs (a few milliseconds) are noisy; use at least tens of thousands of jobs.
- Observation on the development machine: when many measured runs execute inside one process, later runs (around the fifth onward) can be markedly slower than the first ones, while separate processes were stable. The cause was not investigated in this commit. Prefer several short invocations or a small `--runs` value and compare medians.
- Latency statistics are collected with the instrumentation described in section 10; contention analysis (for example lock wait time) is not measured.

---

## 13. Reproducing a Run

1. Use the same build directory configuration (`cmake -S . -B build`; note `CMAKE_BUILD_TYPE` and compiler in `build/CMakeCache.txt`).
2. Close other heavy applications; keep the machine on mains power.
3. Run the exact command line, including `--jobs`, `--workers`, `--scenario`, `--runs` and `--warmup-jobs`.
4. Record the CPU model, OS, compiler version, build type and the full command alongside the printed results.
5. Repeat the whole command several times and compare medians, not single runs.
