# Throughput Benchmarking

`jobq_throughput_benchmark` measures how many jobs per second the real JobQ engine processes end to end. It is the first step of Phase 10 and establishes a **baseline**; it does not optimize anything, and it is not a CI gate (no pass/fail performance threshold exists).

---

## 1. Purpose

- Provide a reproducible way to compare worker counts, workloads and future changes on the same machine.
- Verify correctness while measuring: a run only produces a throughput number if every accepted job completed exactly once.
- Latency percentiles, worker-scaling studies, queue stress and contention analysis are separate future work and are not part of this tool.

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

The benchmark is **not** registered with CTest, so `ctest` never runs it.

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

## 10. Measurement Limitations

- Results depend on CPU model, operating system, compiler and version, **optimization flags** (the default CMake configuration here is `Debug` without `-O`), worker count, workload, system load and thermal state. Do not compare numbers across machines or builds.
- The `noop` and `lightweight` jobs are tiny, so results mostly reflect queue and synchronization overhead; they say little about real handlers.
- The timed region includes job creation and submission on the single submitting thread, so for very cheap jobs the producer can be the bottleneck.
- No persistence is used; real deployments with `FileJobStorage` will be slower.
- Every job stays registered in memory until the run ends, hence the 2,000,000-job cap.
- Short runs (a few milliseconds) are noisy; use at least tens of thousands of jobs.
- Observation on the development machine: when many measured runs execute inside one process, later runs (around the fifth onward) can be markedly slower than the first ones, while separate processes were stable. The cause was not investigated in this commit. Prefer several short invocations or a small `--runs` value and compare medians.
- Only throughput is measured: no latency percentiles, queue wait time or contention analysis.

---

## 11. Reproducing a Run

1. Use the same build directory configuration (`cmake -S . -B build`; note `CMAKE_BUILD_TYPE` and compiler in `build/CMakeCache.txt`).
2. Close other heavy applications; keep the machine on mains power.
3. Run the exact command line, including `--jobs`, `--workers`, `--scenario`, `--runs` and `--warmup-jobs`.
4. Record the CPU model, OS, compiler version, build type and the full command alongside the printed results.
5. Repeat the whole command several times and compare medians, not single runs.
