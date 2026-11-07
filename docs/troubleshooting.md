# Troubleshooting Guide

Practical diagnostics for developers and operators. Every explanation below matches the current implementation. For background see [Reliability](reliability.md) and [Fault Tolerance](fault-tolerance.md).

> **Protect job data.** The journal is the only durable record of accepted jobs. Do not delete or truncate it as a troubleshooting step. Work on a copy:
> ```powershell
> Copy-Item data\jobq.journal data\jobq.journal.bak
> ```
> Read-only inspection commands are `jobq_cli list` and `jobq_cli status`; they only call `FileJobStorage::loadAll()`. `jobq_cli submit` and `jobq_cli run` construct a persistent `JobManager`, which performs recovery and can **append** records (for example failing an interrupted `Running` job and marking it `Pending` for retry). Make a backup before running them against a journal you are unsure about.

---

## 1. Build and Startup

### CMake configuration fails
- **Cause:** CMake older than 3.20, no C++20 compiler, or wrong toolchain on `PATH`. The project requires `cmake_minimum_required(VERSION 3.20)` and C++20.
- **Inspect:** `cmake --version`, `g++ --version`; re-run `cmake -S . -B build` and read the first error.
- **Fix:** Follow the [Installation Guide](installation.md) (MSYS2 MinGW UCRT64). Use a fresh build directory if the cached generator or compiler changed.

### Compiler errors
- **Cause:** Compiler without C++20 support (`std::latch`, `std::osyncstream` and `<syncstream>` are used).
- **Inspect:** the first reported error; confirm the compiler version supports C++20 library features.

### Linker errors (undefined references, `pthread`)
- **Cause:** A new target missing `${JOBQ_CORE_SOURCES}` or `Threads::Threads`. Every test target in `CMakeLists.txt` follows that pattern.
- **Fix:** Copy the registration pattern from an existing target (see [Testing](testing.md#5-adding-a-new-test-target)).

### Test binaries exit immediately with `0xc0000139` (Windows)
- **Cause:** The test process cannot find the MinGW runtime DLLs, typically when `ctest` is launched from a shell whose `PATH` lacks the toolchain `bin` directory.
- **Fix:** Run from the same shell that built the project (for example PowerShell with the toolchain on `PATH`).

### Worker startup failure
- **Behaviour:** `WorkerPool::start()` rethrows the thread-creation error after rolling back: it closes the queue, joins the workers already started and marks the pool stopped. The pool cannot be restarted; create a new `WorkerPool` (and a new `JobQueue`, since the old one is closed).
- **Also:** a worker count of zero throws `std::invalid_argument` from the constructor.

---

## 2. Runtime

### Workers are not picking up jobs
| Check | Meaning |
|---|---|
| Was `pool.start()` called? | Workers only exist after `start()`. A never-started pool executes nothing. |
| Is a handler registered for the job's type? | Otherwise each attempt fails with `No handler registered for job type: X`. |
| Does `pool.isStopped()` return true? | A stopped pool cannot be restarted (`std::logic_error`). |
| Is the queue the same one the manager submits to? | `JobManager` and `WorkerPool` must share one `JobQueue`. |
| Are the jobs delayed retries? | See below. |

### "Job submission failed because the queue is closed"
- **Cause:** `JobQueue::close()` was called, normally by `WorkerPool::stop()`, or by the pool destructor. Closure is permanent.
- **Expected:** The job is not registered, not counted in `submittedJobs`, and with storage a `Cancelled` record is written. It will not run after a restart.
- **Fix:** Create a new queue, manager and pool; do not submit after shutdown starts.

### Jobs remain `Pending`
Possible causes:
1. The pool was never started or was stopped before the job was dequeued. A stopped-before-start pool leaves queued jobs `Pending`; persistent jobs are re-queued by recovery on the next start.
2. A save failed on the `Pending -> Running` transition. The job stays `Pending` in memory and is **not** requeued in this process (see [Fault Tolerance](fault-tolerance.md#persistence-failures-while-workers-run)). `stderr` shows `Job N exhausted its retry policy` or an error line. Fix the storage problem and restart; recovery re-queues it.
3. It is a delayed retry that is not due yet.

**Inspect:** `jobq_cli status <ID> --storage <path>` (read-only).

### Delayed retries appear unavailable
- **Expected:** a retry is invisible to `tryPop()` until `readyAt`. It is counted by `queue.size()` and keeps `queue.empty()` false.
- **Delay** comes from `RetryPolicy`: `initialDelay * multiplier^(attempt-1)` capped at `maxDelay` (the default `maxDelay` is 5000 ms). See [Retry System](retry-system.md).
- A recovered retry restarts its backoff from the moment of recovery, not from the original failure time.

### Job handlers keep failing
- **Inspect:** `stderr` and stdout lines: `Job N failed on attempt K: <message>`, then either `Retry scheduled ... after X ms` or `exhausted its retry policy; final status: FAILED`.
- A job runs at most `1 + maxRetries` times; afterwards it is `Failed` and is not retried again (including after restart).
- Handler exceptions never stop workers.

### Worker shutdown appears blocked
`stop()` closes the queue and then waits. It is waiting for one of:
- a handler that has not returned (no timeout exists);
- a delayed retry still counting down (the longest pending delay);
- a job that keeps failing and retrying until its policy is exhausted.

**Inspect:** worker log lines `[Worker N] picked Job ...` without a matching completed/failed line identify the stuck handler. **Do not** call `stop()` from inside a handler: it throws `std::logic_error` because it would deadlock on its own thread. Retry delays can be shortened only by changing the job's `RetryPolicy` for future submissions.

### Metrics counters differ from the number of unique jobs
This is expected: counters count **attempts**, not jobs.
- `submittedJobs` = accepted submissions; unaffected by retries or recovery.
- `startedExecutions` = attempts (one job with 2 retries that always fails adds 3).
- `finishedExecutions = completedExecutions + failedExecutions`, and after all work drains `startedExecutions == finishedExecutions`.
- `scheduledRetries <= failedExecutions`.
- Metrics are process-local; after a restart they start at zero and recovered jobs do not add to `submittedJobs` (recovered retries do add to `scheduledRetries`).

If `failedExecutions` is higher than expected after storage trouble: an attempt whose `Completed` snapshot could not be saved is counted as failed even though the handler returned.

---

## 3. Persistence

### Storage file cannot be opened
- **Messages:** `Unable to open job storage file for reading` / `for writing`; `Storage file path cannot be empty`.
- **Cause:** permissions, a path that is a directory, a locked file, or no space.
- **Inspect:** check the path and permissions; `Test-Path`, `Get-Item`. A missing file is **not** an error on load; it is treated as an empty store, and it is created on the first save.
- **Note:** a read error is reported as an error and is never treated as "no jobs".

### Storage file contains malformed records
- **Message:** `Invalid job storage record at line N: <reason>`, or `Empty record in job storage at line N`.
- **Policy:** fail fast. No jobs are scheduled and the journal is not modified by the failed load.
- **Inspect (read-only):** `Get-Content data\jobq.journal | Select-Object -Index (N-1)` to view line N. Record format: `JOBQ1|id|STATUS|PRIORITY|attempts|maxRetries|initialDelayMs|multiplier|maxDelayMs|hex(type)|hex(payload)`.
- **Causes:** manual edits, truncation in the middle of the file, a different writer, a record failing validation (ID 0, unknown status/priority, multiplier below 1, attempts beyond the retry policy).
- **Fix:** work on a copy; correct or remove only the damaged line in the copy, verify with `jobq_cli list --storage <copy>`, then swap files. An unterminated **final** line is different: it is treated as a torn write, ignored on load and truncated on the next save.

### Recovery fails
| Message | Meaning |
|---|---|
| `Stored job N is invalid: ...` | Record failed `validateJobRecord()`. |
| `Storage returned duplicate job IDs: N` | A custom `JobStorage` returned the same ID twice (the file storage collapses repeats to the latest record). |
| `Stored job ID prevents generating another unique ID` | A record has ID `UINT64_MAX`. |
| `Failed to enqueue recovered job N` | The target queue was already closed. |
| `injected/IO error text` from `save()` | Recovery persists transitions for interrupted jobs (`Running -> Failed -> Pending`); a failing write aborts recovery. |

Recovery validates **every** record before it creates a job or writes anything, so a failure leaves the journal as it was.

### Job ID conflict is detected
`JobManager` rejects duplicates it receives from the storage layer. In a journal, several lines with the same ID are normal (one line per state change; the latest wins). If you see the error, you are using a non-file `JobStorage`, or two sources were merged. Do not edit IDs; fix the source that produced duplicates.

### Storage write operation fails
- **Messages:** `Failed to write job record to storage file`, `Unable to open job storage file for writing`.
- **Expected:** `submit()` rolls back (no registry entry, nothing queued, not counted); a failure during a state transition leaves the job in its previous state and is logged on `stderr`.
- **Caveats:** jobs may stay `Pending` or `Running` in memory until restart; a job whose `Completed` write failed can run again after recovery (at-least-once). Make handlers idempotent.

### Journal keeps growing
Expected: one line per state change and no compaction yet ([Roadmap](roadmap.md)). Do not delete the file to reclaim space while it holds pending or unfinished jobs.

### Two processes use one journal
Unsupported. `FileJobStorage` uses an in-process mutex only, with no file locking or cross-process coordination. Concurrent writers can interleave lines.
