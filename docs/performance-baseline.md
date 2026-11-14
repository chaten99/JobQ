# JobQ performance baseline and regression report (10.6)

Measured on 14 November 2026 (Asia/Calcutta), against baseline commit `8020159`.
This report describes one Windows development machine, not universal JobQ performance.

## Environment and method

- Intel Core i5-13450HX, 10 cores / 16 logical processors; Windows 11 Home Single Language 10.0.26300. Shared interactive host; affinity, power state and thermals were not controlled.
- MSYS2 MinGW UCRT64 GCC 16.2.0 (Rev4), CMake 4.4, MinGW Makefiles, C++20; Release flags `-O3 -DNDEBUG -std=c++20`.
- Existing `build` remained Debug. Independent Release directory: `build/perf-release`. The original source was also exported from `8020159` to an ignored directory and built with identical Release settings for alternating A/B confirmation.
- All main configurations use 100,000 jobs, 1,000 unmeasured warmup jobs, three measured runs. Throughput/latency: one producer, four workers, `noop`, `lightweight`, `mixed`. Scaling: one producer, workers 1,2,4,8, `lightweight`. Stress: four producers/four workers, `concurrent`, `priority-retry`, `failure`, `sustained`; default no-op workload unless the scenario fixes it.
- The initial complete suite ran before the production edit, then ran again with identical arguments after it. Lock diagnostics were disabled except for the explicit `compare` runs. Throughput-only runs additionally use `--no-latency`; latency and stress measurements include the existing observer overhead. Per-job logging is redirected by the existing benchmarks but formatting/synchronization still execute.
- Throughput uses completed jobs divided by elapsed time through drain/join. Failure scenarios complete 90,000 jobs, permanently fail 10,000, execute 130,000 attempts, fail 40,000 attempts and schedule 30,000 retries per run. These denominators are not interchangeable.
- Latency tables use the benchmark's pooled samples (300,000 successful attempts for throughput scenarios), with exact nearest-rank percentiles. Do not compare latency-enabled throughput against throughput-only measurements.
- Percentage change is `(after - before) / before * 100`; negative durations indicate improvement. A zero baseline would be reported as n/a rather than divided by zero. Values reflect rounded benchmark output.
- Raw logs, temporary probes, binaries and exported original source remain under ignored `build/`; none are part of the patch. Relevant measurements and the complete probe source are preserved below.

## Inspection, bottlenecks and plan

Commits 10.1–10.5 and all required tools are present. Inspected Job, queue, executor, worker/pool, manager, metrics, latency sampling, contention collection, benchmark runners, CMake and existing regression tests. No required profiling dependency was missing. The initial working tree was clean; branch `perf/performance-optimization` was left unchanged.

The 100,000-job concurrent diagnostic baseline recorded queue wait of **2.391 s**, 42.63% of aggregate producer/worker thread time, 34.96% blocked acquisitions, 186 ns average hold, 3.99 us average acquisition wait. Registry wait share was 5.87%, metrics 1.42%, executor 0.34%. This identifies the queue as the first synchronization path to investigate, but does not prove all queue work should be changed.

The plan recorded before the production edit was to test exactly one optimization: cache immutable job priority in ready-heap entries. `QueueEntryCompare` previously dereferenced two shared job objects and called the separately compiled `getPriority()` on every heap comparison under the queue mutex. A prefilled mixed-priority probe isolated heap cost before modifying production code. Preserve priority and FIFO order, due-retry timestamps/promotion order, accepted-work accounting, closure/draining and shared ownership. Build, run focused correctness tests, repeat the probe and full benchmarks, and retain the patch only with a repeatable measured benefit. No scheduling structure or synchronization policy would change.

Registry, metrics and executor locks were investigated through the same diagnostic data; their lower wait shares did not justify changing their synchronization. Worker logging remains a possible cost, but no isolated experiment justified a change. Allocation, snapshot copies and empty-retry clock reads were inspected but not optimized without causal measurements.

## Accepted optimization

Only `include/queue/JobQueue.h` and `src/queue/JobQueue.cpp` change production behavior internally. Ready entries cache `JobPriority`; ordinary pushes, immediate retries and delayed promotions all initialize it. Job priority has no mutator, so the cached key cannot become stale. Sequence generation and all comparisons/timestamps/notifications retain their scheduling meaning. Public APIs and persistence are unchanged.

Trade-off: on this 64-bit ABI a ready entry grows from 24 to 32 bytes (8 extra bytes, 33.3% per entry; approximately 0.76 MiB for 100,000 live ready entries, excluding vector spare capacity). Delayed entries are unchanged. This accepts modest bounded extra queue storage for less heap-comparison work. It does not bound the manager registry, which retains jobs as before.

## Initial identical-configuration suite

These runs are useful regression observations; the wider distributions and later A/B results matter more than any isolated percentage.

| Scenario / collection | Mean before | Mean after | Change | Median before | Median after | Change | Before min–max | After min–max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| throughput-noop | 162206.11 | 150446.99 | -7.25% | 153488.95 | 150914.72 | -1.68% | 145091.19–188038.18 | 148621.58–151804.68 |
| throughput-lightweight | 167624.08 | 160806.16 | -4.07% | 158978.85 | 159565.47 | 0.37% | 149879.42–194013.97 | 156813.33–166039.67 |
| throughput-mixed | 176314.25 | 158385.93 | -10.17% | 181350.07 | 158149.90 | -12.79% | 161717.57–185875.13 | 148070.38–168937.50 |
| latency-noop | 154664.03 | 140695.03 | -9.03% | 156552.73 | 138813.70 | -11.33% | 141444.24–165995.13 | 135246.46–148024.93 |
| latency-lightweight | 159480.62 | 151278.92 | -5.14% | 159574.20 | 146109.46 | -8.44% | 154888.55–163979.10 | 145434.70–162292.61 |
| latency-mixed | 162808.53 | 157549.63 | -3.23% | 158879.38 | 155276.42 | -2.27% | 152735.93–176810.27 | 149428.14–167944.33 |
| stress-concurrent | 469561.66 | 503651.12 | 7.26% | 454272.89 | 536047.88 | 18.00% | 421582.32–532829.77 | 434247.16–540658.32 |
| stress-priority-retry | 252212.19 | 273081.70 | 8.27% | 256920.88 | 272509.68 | 6.07% | 241894.84–257820.85 | 270852.64–275882.79 |
| stress-failure | 265654.62 | 282341.31 | 6.28% | 265920.04 | 281146.42 | 5.73% | 262815.54–268228.27 | 278324.63–287552.89 |
| stress-sustained | 467350.09 | 455655.04 | -2.50% | 456671.26 | 468251.82 | 2.54% | 455080.80–490298.22 | 426123.96–472589.35 |

All throughput values are jobs/s. `throughput-*` disables latency; `latency-*` enables it. Stress retains latency. All configurations passed all three runs.

### Pooled latency before/after

| Scenario | Metric | Before (us) | After (us) | Change |
|---|---|---:|---:|---:|
| noop | Queue Waiting Time Average | 147.90 | 174.24 | 17.81% |
| noop | Queue Waiting Time P50 | 4.40 | 4.40 | 0.00% |
| noop | Queue Waiting Time P95 | 23.90 | 11.40 | -52.30% |
| noop | Queue Waiting Time P99 | 6485.00 | 6619.00 | 2.07% |
| noop | Execution Duration (successful attempts) Average | 1.11 | 1.10 | -0.90% |
| noop | Execution Duration (successful attempts) P50 | 1.20 | 1.20 | 0.00% |
| noop | Execution Duration (successful attempts) P95 | 1.90 | 1.80 | -5.26% |
| noop | Execution Duration (successful attempts) P99 | 2.70 | 2.50 | -7.41% |
| lightweight | Queue Waiting Time Average | 7.41 | 9.74 | 31.44% |
| lightweight | Queue Waiting Time P50 | 4.20 | 4.30 | 2.38% |
| lightweight | Queue Waiting Time P95 | 10.00 | 11.50 | 15.00% |
| lightweight | Queue Waiting Time P99 | 82.30 | 117.00 | 42.16% |
| lightweight | Execution Duration (successful attempts) Average | 1.42 | 1.40 | -1.41% |
| lightweight | Execution Duration (successful attempts) P50 | 1.50 | 1.50 | 0.00% |
| lightweight | Execution Duration (successful attempts) P95 | 2.20 | 2.10 | -4.55% |
| lightweight | Execution Duration (successful attempts) P99 | 3.10 | 3.30 | 6.45% |
| mixed | Queue Waiting Time Average | 8.17 | 5.18 | -36.60% |
| mixed | Queue Waiting Time P50 | 4.40 | 4.10 | -6.82% |
| mixed | Queue Waiting Time P95 | 17.80 | 8.60 | -51.69% |
| mixed | Queue Waiting Time P99 | 150.00 | 39.30 | -73.80% |
| mixed | Execution Duration (successful attempts) Average | 1.62 | 1.63 | 0.62% |
| mixed | Execution Duration (successful attempts) P50 | 1.40 | 1.40 | 0.00% |
| mixed | Execution Duration (successful attempts) P95 | 3.60 | 3.60 | 0.00% |
| mixed | Execution Duration (successful attempts) P99 | 4.70 | 4.40 | -6.38% |

### Scaling: before

```text
Scaling Comparison (speedup uses median throughput; baseline = 1 worker)

Workers    Median jobs/s    Mean jobs/s   Speedup   Efficiency  Change vs 1 worker
1              649485.61      658927.73     1.00x      100.00%  +0.00 jobs/sec (+0.00%)
2              407994.57      418346.72     0.63x       31.41%  -241491.04 jobs/sec (-37.18%)
4              150911.05      202304.79     0.23x        5.81%  -498574.56 jobs/sec (-76.76%)
8              262183.54      250672.09     0.40x        5.05%  -387302.07 jobs/sec (-59.63%)

Latency Comparison (pooled over successful runs; nearest-rank percentiles)

Workers          Queue wait avg / p50 / p95 / p99             Execution avg / p50 / p95 / p99
1        23.607 ms / 25.097 ms / 43.308 ms / 44.758 ms          731 ns / 700 ns / 900 ns / 1.30 us
2        13.90 us / 3.60 us / 40.10 us / 328.00 us        1.03 us / 800 ns / 1.80 us / 2.50 us
4        127.35 us / 6.90 us / 707.20 us / 1.988 ms       1.83 us / 1.60 us / 3.70 us / 5.70 us
8        1.037 ms / 832.20 us / 2.598 ms / 3.308 ms       2.55 us / 1.80 us / 3.80 us / 6.40 us
```

### Scaling: after

```text
Scaling Comparison (speedup uses median throughput; baseline = 1 worker)

Workers    Median jobs/s    Mean jobs/s   Speedup   Efficiency  Change vs 1 worker
1              748186.58      721011.99     1.00x      100.00%  +0.00 jobs/sec (+0.00%)
2              466434.88      451714.03     0.62x       31.17%  -281751.70 jobs/sec (-37.66%)
4              144365.29      183924.09     0.19x        4.82%  -603821.30 jobs/sec (-80.70%)
8              256948.60      254035.72     0.34x        4.29%  -491237.99 jobs/sec (-65.66%)

Latency Comparison (pooled over successful runs; nearest-rank percentiles)

Workers          Queue wait avg / p50 / p95 / p99             Execution avg / p50 / p95 / p99
1        25.333 ms / 27.807 ms / 39.991 ms / 41.701 ms         709 ns / 700 ns / 1.00 us / 1.20 us
2        20.69 us / 4.00 us / 112.80 us / 304.10 us        1.00 us / 800 ns / 1.80 us / 2.50 us
4        436.04 us / 6.50 us / 2.343 ms / 4.160 ms       1.71 us / 1.60 us / 2.50 us / 4.60 us
8        881.55 us / 667.00 us / 2.494 ms / 3.117 ms       2.60 us / 1.60 us / 3.40 us / 5.90 us
```

## Alternating A/B confirmation

After all builds/tests finished, seven fresh processes per configuration and version ran sequentially, alternating original/changed order by round. Each process used one measured run, 1,000 warmup jobs and 100,000 measured jobs. Throughput scenarios disable latency, stress scenarios retain it. Original binaries were built from an untouched export of `8020159` with the same compiler, generator and flags. No compilation or tests ran concurrently with these measurements.

| Scenario | Mean before | Mean after | Change | Median before | Median after | Change | Before min–max | After min–max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| noop | 152642.53 | 157992.99 | 3.51% | 153010.93 | 157758.94 | 3.10% | 140824.96–166769.17 | 128167.65–176802.36 |
| lightweight | 165303.68 | 164100.56 | -0.73% | 167860.69 | 168055.28 | 0.12% | 153256.54–175219.23 | 134380.59–192988.72 |
| mixed | 171178.55 | 179073.15 | 4.61% | 173595.68 | 177768.90 | 2.40% | 155680.53–179330.99 | 150473.94–207848.27 |
| concurrent | 529727.99 | 565740.58 | 6.80% | 523823.49 | 561081.90 | 7.11% | 443059.54–570067.25 | 497625.58–622275.21 |
| priority-retry | 254695.14 | 270104.80 | 6.05% | 259304.72 | 267192.34 | 3.04% | 187630.68–286717.07 | 245618.71–300680.84 |
| failure | 257061.11 | 274122.07 | 6.64% | 269331.09 | 278051.45 | 3.24% | 186501.05–285592.16 | 206032.31–327032.98 |

### Individual A/B throughput samples (jobs/s)

- noop before: 153289.27, 166769.17, 153010.93, 147374.75, 155917.77, 140824.96, 151310.88
- noop after: 153164.48, 176802.36, 157758.94, 176721.50, 163609.62, 128167.65, 149726.35
- lightweight before: 164192.99, 167860.69, 169992.80, 170476.88, 175219.23, 156126.64, 153256.54
- lightweight after: 158764.18, 171181.08, 155077.37, 168055.28, 168256.69, 134380.59, 192988.72
- mixed before: 173595.68, 179330.99, 174534.23, 167037.69, 175716.75, 155680.53, 172353.97
- mixed after: 174917.83, 177768.90, 207848.27, 193282.54, 166598.25, 150473.94, 182622.35
- concurrent before: 523823.49, 566245.64, 521641.61, 570067.25, 566680.12, 516578.29, 443059.54
- concurrent after: 561081.90, 596615.99, 573940.46, 558529.73, 550115.22, 497625.58, 622275.21
- priority-retry before: 267161.73, 277763.03, 256092.37, 286717.07, 248196.37, 187630.68, 259304.72
- priority-retry after: 286670.22, 276326.30, 267192.34, 300680.84, 264865.42, 245618.71, 249379.74
- failure before: 272043.37, 269331.09, 282090.63, 285592.16, 242761.79, 186501.05, 261107.67
- failure after: 278051.45, 311749.74, 278745.56, 327032.98, 240503.39, 206032.31, 276739.07

### Stress latency confirmation

All values below are microseconds. Each row summarizes the seven run-level values, **not pooled percentiles**. Average queue wait and execution duration are means of per-run averages (equal sample counts). Tail entries are medians of per-run percentiles, with min–max to show variation. Execution success/failure samples remain separate.

| Scenario | Metric | Before summary | After summary | Change in summary | Before min–max | After min–max |
|---|---|---:|---:|---:|---:|---:|
| concurrent | Queue Wait avg | 26.49 | 30.01 | 13.29% | 21.10–48.10 | 13.84–73.08 |
| concurrent | Queue Wait p50 | 10.50 | 11.60 | 10.48% | 7.90–14.60 | 7.10–13.40 |
| concurrent | Queue Wait p95 | 88.40 | 75.30 | -14.82% | 66.10–258.60 | 52.50–566.10 |
| concurrent | Queue Wait p99 | 246.10 | 237.30 | -3.58% | 132.10–543.30 | 99.30–731.10 |
| concurrent | Exec (ok) avg | 0.96 | 0.95 | -1.22% | 0.80–1.04 | 0.82–1.00 |
| concurrent | Exec (ok) p50 | 0.60 | 0.60 | 0.00% | 0.50–0.70 | 0.50–0.70 |
| concurrent | Exec (ok) p95 | 2.00 | 2.00 | 0.00% | 1.60–2.40 | 1.70–2.20 |
| concurrent | Exec (ok) p99 | 4.20 | 3.90 | -7.14% | 2.80–4.70 | 3.10–4.90 |
| priority-retry | Queue Wait avg | 39602.00 | 35999.86 | -9.10% | 32384.00–57904.00 | 32910.00–40825.00 |
| priority-retry | Queue Wait p50 | 52.00 | 49.00 | -5.77% | 40.80–66.60 | 36.80–64.60 |
| priority-retry | Queue Wait p95 | 133657.00 | 127826.00 | -4.36% | 126488.00–177231.00 | 118611.00–149275.00 |
| priority-retry | Queue Wait p99 | 145829.00 | 139963.00 | -4.02% | 138573.00–186285.00 | 129139.00–160543.00 |
| priority-retry | Exec (ok) avg | 0.83 | 0.86 | 2.71% | 0.74–1.06 | 0.74–1.02 |
| priority-retry | Exec (ok) p50 | 0.60 | 0.60 | 0.00% | 0.50–0.80 | 0.50–0.70 |
| priority-retry | Exec (ok) p95 | 1.50 | 1.80 | 20.00% | 1.40–2.80 | 1.40–2.30 |
| priority-retry | Exec (ok) p99 | 2.80 | 3.00 | 7.14% | 1.90–4.30 | 1.90–5.40 |
| priority-retry | Exec (failed) avg | 5.90 | 5.67 | -3.83% | 5.36–8.64 | 5.23–6.25 |
| priority-retry | Exec (failed) p50 | 5.10 | 5.10 | 0.00% | 5.00–7.90 | 5.00–5.50 |
| priority-retry | Exec (failed) p95 | 7.40 | 8.30 | 12.16% | 7.00–14.60 | 6.80–10.80 |
| priority-retry | Exec (failed) p99 | 12.20 | 13.20 | 8.20% | 9.80–22.20 | 8.20–23.70 |
| failure | Queue Wait avg | 66553.14 | 61782.43 | -7.17% | 59143.00–93359.00 | 49531.00–83911.00 |
| failure | Queue Wait p50 | 67954.00 | 62191.00 | -8.48% | 65380.00–112338.00 | 51560.00–87997.00 |
| failure | Queue Wait p95 | 93628.00 | 95286.00 | 1.77% | 90330.00–142977.00 | 85367.00–153088.00 |
| failure | Queue Wait p99 | 94389.00 | 95540.00 | 1.22% | 90460.00–143523.00 | 86498.00–153878.00 |
| failure | Exec (ok) avg | 0.88 | 0.89 | 1.81% | 0.75–1.16 | 0.73–1.25 |
| failure | Exec (ok) p50 | 0.60 | 0.60 | 0.00% | 0.50–0.80 | 0.50–0.90 |
| failure | Exec (ok) p95 | 1.50 | 1.70 | 13.33% | 1.40–3.00 | 1.40–3.10 |
| failure | Exec (ok) p99 | 3.00 | 3.00 | 0.00% | 1.90–4.50 | 1.90–4.50 |
| failure | Exec (failed) avg | 6.37 | 6.51 | 2.20% | 5.40–8.50 | 5.26–9.20 |
| failure | Exec (failed) p50 | 5.40 | 5.40 | 0.00% | 5.20–7.30 | 5.10–8.70 |
| failure | Exec (failed) p95 | 9.30 | 10.10 | 8.60% | 7.00–15.70 | 6.70–15.60 |
| failure | Exec (failed) p99 | 15.30 | 15.70 | 2.61% | 11.60–21.60 | 8.30–21.20 |

## Queue-only confirmation

This supplemental probe isolates an accumulated ready backlog, rather than duplicating engine stress tests. It precreates 100,000 jobs, evenly cycles Low/Normal/High priority, then times pushing all jobs and draining them in one thread. Timed work includes queue-ready clocks, queue locking, heap operations and output-ID vector storage; creation and expected-order verification are outside the timer. It verifies every ID in exact priority/FIFO order. One warmup plus seven measured runs per process; three alternating original/changed process batches. No latency observer, contention collector, workers, manager or handler logging is involved. It measures jobs through a push-plus-pop pair per second, not individual queue operations/s or engine throughput.

The first exploratory changed-probe invocation overlapped compilation and was slower; it is excluded from conclusions as a contaminated measurement. The later three comparisons below ran after compilation completed. Both original and changed batch summaries are shown; no measured run within these batches is excluded.

| Batch | Before mean | After mean | Change | Before median | After median | Change | Before min–max | After min–max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2891387 | 3694263 | 27.77% | 2917920 | 3724370 | 27.64% | 2594650–3068840 | 3480090–3864480 |
| 2 | 2911809 | 3523793 | 21.02% | 2887330 | 3290460 | 13.96% | 2722950–3153930 | 3079340–4131040 |
| 3 | 2699741 | 3705004 | 37.24% | 2667020 | 3728890 | 39.81% | 2545940–2852250 | 3579700–3816950 |

Probe source (save under the ignored build directory):

```cpp
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>
#include "queue/JobQueue.h"
int main() {
    constexpr int n = 100000;
    std::vector<std::shared_ptr<jobq::Job>> jobs;
    for (int i = 0; i < n; ++i) jobs.push_back(std::make_shared<jobq::Job>(i+1,"T","p",static_cast<jobq::JobPriority>(i%3)));
    for (int run = 0; run < 8; ++run) {
        jobq::JobQueue queue;
        auto start = std::chrono::steady_clock::now();
        for (auto& j : jobs) if (!queue.push(j)) return 1;
        std::vector<unsigned long long> ids;
        ids.reserve(n);
        while (auto j = queue.tryPop()) ids.push_back(j->getId());
        auto end = std::chrono::steady_clock::now();
        std::vector<unsigned long long> expected;
        for (int priority = 2; priority >= 0; --priority)
            for (int i = 0; i < n; ++i) if (i%3 == priority) expected.push_back(i+1);
        if (ids != expected || !queue.empty()) return 2;
        if (run) std::cout << n/std::chrono::duration<double>(end-start).count() << '\n';
    }
}
```

Compile each source version with:

```powershell
g++ -O3 -DNDEBUG -std=c++20 -I include build/perf-results/queue-probe.cpp src/queue/JobQueue.cpp src/job/Job.cpp src/metrics/ContentionMetrics.cpp -o build/perf-results/queue-probe.exe
```

## Diagnostic comparison and instrumentation overhead

Concurrent scenario, 100,000 jobs, four producers/four workers, three runs per mode. These numbers include latency instrumentation; only the enabled mode includes contention recording. Lock-wait percentiles are histogram upper bounds, not exact latency percentiles. Condition-variable reacquisition is not measured as lock acquisition wait.

| Diagnostic statistic | Before | After |
|---|---:|---:|
| Disabled median throughput (jobs/s) | 585284.43 | 515812.49 |
| Enabled median throughput (jobs/s) | 428589.49 | 471300.62 |
| Enabled vs disabled throughput | -26.77% | -8.63% |
| Queue acquisitions | 600015 | 600015 |
| Queue blocked acquisitions | 209753 (34.96%) | 205288 (34.21%) |
| Queue total wait | 2.391 s | 2.048 s |
| Queue average / maximum acquisition wait | 3.99 us / 533.00 us | 3.41 us / 1.079 ms |
| Queue estimated p50 / p99 acquisition wait | 0 ns / 65.53 us | 0 ns / 65.53 us |
| Queue average hold | 186 ns | 174 ns |
| Queue hold samples | 627947 | 624860 |
| Queue wait share of aggregate thread time | 42.63% | 40.21% |
| Registry average wait | 1.10 us | 1.05 us |

The enabled run shows shorter mean queue holds and waits, but its maximum wait grew; this does not establish improved worst-case bounds. The disabled concurrent result in this particular suite regressed, unlike the alternating no-op concurrent stress confirmation. Workloads differ (contention concurrent uses lightweight handlers), and distributions vary. Diagnostics alter both cost and scheduling; enabled throughput is never used as primary production-throughput evidence. Calibration before/after measured plain mutex at 11.2/11.2 ns, disabled wrapper at 11.3/11.2 ns and enabled wrapper at 81.3/81.7 ns per operation. The diagnostics remain optional and unchanged.

## Conclusions, trade-offs and remaining work

Retain the single cached-priority optimization for the repeatable queue-backlog benefit, supported by the alternating concurrent/retry stress observations. Queue-only median gains are substantially larger than engine gains; do not advertise them as engine speedups. In seven-process A/B confirmation, concurrent stress median rose 7.11%, priority-retry 3.04%, and failure 3.24%, with overlapping ranges. No-op and mixed single-producer medians rose only 3.10% and 2.40%; lightweight changed 0.12%. Those small differences do not establish a reliable single-producer benefit. The initial suite's single-producer regressions did not persist in alternating throughput-only confirmation; report them rather than hiding the noise.

Latency is not uniformly better. Concurrent mean queue wait increased 13.29% (26.49 to 30.01 us), while median per-run p95 decreased 14.82%. Priority-retry mean queue wait decreased 9.10%, but successful-attempt median per-run execution p95 increased from 1.50 to 1.80 us (+20%), and failed-attempt p95 from 7.40 to 8.30 us (+12.16%). The execution timing includes shared metrics, logging and handler lookup; changing dequeue throughput can change downstream scheduling and overlap. The wide overlapping run ranges (shown above) do not isolate a causal execution-tail regression. Failure queue p95 rose 1.77%, with overlapping ranges. No tail-latency guarantee is claimed. Initial lightweight pooled queue p99 rose 42.16% (82.30 to 117 us); queue tails are sensitive to scheduling and backlog, so further controlled latency runs would be needed for a tail-performance claim.

Scaling still favors one worker for these tiny handlers. In the initial scaling suite median throughput increased at one/two workers and decreased at four/eight; four-worker pooled queue p95 increased from 707.20 us to 2.343 ms. More workers do not guarantee higher throughput. These results and the initial sustained mean/median moving in opposite directions illustrate measurement variability, not a solved scaling problem.

Remaining bottlenecks include shared queue synchronization, registry insertion, producer scheduling and existing worker log formatting; allocator/cache behavior and logging cost have not been isolated. No custom allocator, lock-free structure, metrics approximation or broader architecture change is justified by this experiment. The eight-byte ready-entry memory cost is the explicit implementation trade-off.

## Correctness and resource verification

- `cmake -S . -B build` and `cmake --build build -j 8`: passed, existing MinGW Debug configuration preserved.
- `ctest --test-dir build --output-on-failure`: **28/28 passed**, 14.99 s.
- Independent Release configure/build and `ctest --test-dir build/perf-release --output-on-failure`: **28/28 passed**, 11.57 s.
- Immediately after the production edit, focused Release priority, backoff, queue reliability and latency tests: **4/4 passed**.
- Added `testMixedInsertionOrdering` to the existing queue reliability test: 3,000 mixed-priority ordinary/zero-delay retry insertions plus three delayed retries; exact priority/FIFO order through heap growth/promotion and closed-queue drain. It asserts correctness, never throughput.
- Every initial/changed benchmark configuration and all 84 A/B measured runs passed built-in integrity checks. Unique IDs, registry object identity/counts, expected status/attempt counts, exact execution counts, checksums, metrics/latency sums, empty queue and stopped pool were checked by the existing runners. No deadlock or lost/duplicate execution was observed.
- Priority and state ownership remain under existing mutexes; cached priority is immutable. No new resources, threads or ownership relationships are introduced. Existing lifecycle, shutdown, fault-injection, persistence/recovery and concurrent metrics tests passed. This is evidence, not proof of absence of all races/leaks.
- Attempted `g++ -O1 -g '-fsanitize=address,undefined' ...`: linker failed with `cannot find -lasan` and `cannot find -lubsan`. This installed UCRT64 environment cannot run that sanitizer configuration. No sanitizer, race-detector, leak or long-duration soak success is claimed.

## Reproduction commands

Use identical source versions/configurations, with all compilation finished before timing. Preserve original binaries separately for A/B; the original commit is `8020159`. All commands below run from the appropriate source root. No staging or commit is required.

```powershell
cmake -S . -B build/perf-release -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release
cmake --build build/perf-release -j 8
ctest --test-dir build/perf-release --output-on-failure
foreach ($scenario in @('noop','lightweight','mixed')) {
    ./build/perf-release/jobq_throughput_benchmark.exe --jobs 100000 --workers 4 --scenario $scenario --runs 3 --warmup-jobs 1000 --no-latency
    ./build/perf-release/jobq_throughput_benchmark.exe --jobs 100000 --workers 4 --scenario $scenario --runs 3 --warmup-jobs 1000
}
./build/perf-release/jobq_throughput_benchmark.exe --jobs 100000 --worker-counts 1,2,4,8 --scenario lightweight --runs 3 --warmup-jobs 1000
foreach ($scenario in @('concurrent','priority-retry','failure','sustained')) {
    ./build/perf-release/jobq_queue_stress_benchmark.exe --jobs 100000 --producers 4 --workers 4 --scenario $scenario --runs 3 --warmup-jobs 1000
}
./build/perf-release/jobq_contention_benchmark.exe --jobs 100000 --producers 4 --workers 4 --scenario concurrent --runs 3 --warmup-jobs 1000 --diagnostics compare
```

For the seven-process confirmation use the same throughput and stress commands with `--runs 1`, retain all other flags, omit `sustained`, and invoke each configuration seven times against both binaries. Use original/changed order in odd rounds and changed/original in even rounds. Aggregate throughput samples across processes; summarize latency quantiles as run-level values, not invented pooled percentiles.
