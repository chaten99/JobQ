# CLI Reference Guide

`jobq_cli` is the command-line interface for the JobQ engine. It allows users to submit tasks, inspect job registries, execute worker pools from persistent storage, and export runtime metrics.

## Interactive runtime

Launch a persistent terminal session with the explicit interactive command.

```powershell
.\build\jobq_cli.exe interactive
```

The explicit entry point preserves the existing missing-command usage error.
The `jobq` executable remains the lifecycle demonstration. `jobq_cli --help`
lists one-shot commands and the interactive entry point, while
`jobq_cli interactive --help` prints interactive invocation help without starting
workers. The interactive invocation accepts no other command-line arguments.

```text
JobQ Interactive Runtime
Engine initialized successfully.
Runtime started.
jobq> help
Available commands:
  exit                                    Stop the runtime and exit.
  help [command]                          Display available commands.
  job <job-id>                            Inspect one available job by its identifier.
  jobs                                    List available jobs in ascending identifier order.
  metrics                                 Display cumulative Engine processing metrics.
  monitor [<interval-ms> <sample-count>]  Sample metrics (defaults: 1000 ms, 5 samples; bounded to 60 s of waits).
  queue                                   Display waiting jobs and execution counters.
  quit                                    Alias for exit.
  status                                  Display runtime status.
  submit <type> <payload>                 Submit a job to a registered handler; quote multi-word payloads.
  workers                                 Display worker pool information.
jobq> exit
Shutting down JobQ...
Runtime stopped.
```

The Engine may emit its existing worker-stop diagnostics during shutdown.
Initialization and startup messages follow the operations they report. If
construction or startup fails, the runtime reports an error, returns exit code 1,
and does not enter a partially initialized command loop. One session owns one
Engine for its entire lifetime. Its default configuration is four workers with
in-memory storage. The standalone interactive entry point registers no handlers or demonstration jobs.
Session persistence means the same Engine stays alive across terminal commands.

### Available session commands

| Command | Effect |
|---|---|
| `help [command]` | Lists the registry, or displays usage for one registered command. |
| `status` | Reports runtime and Engine states, configured workers, submitted jobs, and uptime. |
| `workers` | Reports worker pool state and configured worker count. |
| `queue` | Reports waiting queue entries and historical execution counters. |
| `submit <type> <payload>` | Submits a job to a registered handler using default JobOptions. |
| `jobs` | Lists available live/recovered records ordered by ascending ID. |
| `job <job-id>` | Shows one record using a positive decimal uint64 ID. |
| `metrics` | Displays the Engine's cumulative processing metrics. |
| `monitor [<interval-ms> <sample-count>]` | Displays a baseline and a bounded series of cumulative snapshots and interval changes. |
| `exit` | Leaves the command loop and shuts down the Engine. |
| `quit` | Performs the same action as `exit`. |

Only `help` accepts an optional command name, such as `help status`,
`help workers`, or `help queue`. Use `help submit`, `help jobs`, and `help job`
for job commands. `submit` requires exactly two arguments and `job` exactly one;
`jobs`, `status`, `workers`, `queue`, `metrics`, `exit`, and `quit` accept no arguments.
`monitor` accepts either no arguments or exactly two bounded positive integers.
Use `help metrics` and `help monitor` for their registered usage.
Invalid inspection arguments and unknown help topics display usage errors.
Empty or whitespace-only lines are ignored.
Unknown names, malformed input, and command errors are reported on stderr, and
the next prompt remains available. Names are case-sensitive. A session that
handles command errors and then exits cleanly returns 0. Terminal I/O, startup,
or shutdown failures return 1. Invalid invocation arguments return 2.

EOF follows the same graceful shutdown path as exit. For an ordinary Windows
console this is typically Ctrl+Z followed by Enter; Unix-style terminals commonly
use Ctrl+D. Piped input is supported. Input is read one line at a time with
standard streams. Stream failures report an error and trigger cleanup. Only the
first exit request is processed; any remaining piped lines are left unread.
The existing Engine destructor provides its normal idempotent shutdown backstop.

### Inspection fields and consistency

`status` reports runtime lifecycle separately from Engine lifecycle. Both expose
Created, Starting, Running, Stopping, and Stopped states; the runtime can also
report Failed after startup or shutdown failure. Before Engine construction,
Engine information is Unavailable. Uptime uses the runtime's steady clock from
successful startup, freezes after cleanup, and displays as hours:minutes:seconds.
It is Unavailable before successful startup.

`workers` reads the pool's lifecycle flags and configured worker count. This
count describes configuration rather than live or active threads. Active/idle
counts and per-worker identifiers or states are Unavailable. Engine configuration
rejects zero workers.

`queue` distinguishes current waiting entries from historical execution counters:

| Field | Meaning |
|---|---|
| Queue state | Whether the queue is open or closed to new entries. |
| Ready waiting | Entries in the ready priority queue. |
| Retry waiting | Entries in the delayed retry heap. |
| Waiting total | Sum of both counts from the same queue snapshot. |
| Completed executions | Historical successful execution attempts. |
| Failed executions | Historical failed execution attempts, including attempts later retried. |

Workers remove entries before execution, so waiting counts exclude active work.
A due retry remains in the retry heap until normal queue retrieval promotes it.
Inspection never promotes, removes, or reschedules entries. Execution counters
are neither current queue depth nor counts of distinct jobs. Queue capacity and
active execution counts are omitted because the implementation exposes no such
queue statistics.

`Engine::snapshot()`, `WorkerPool::snapshot()`, and `JobQueue::snapshot()` return
value copies under their existing component locks. Runtime lifecycle and uptime
are also synchronized. Formatting happens after locks are released. A combined
report samples components at nearby instants; it is not a globally atomic view.
Snapshots remain available after shutdown while the owning runtime exists.
Callers must finish concurrent inspection before destroying that owner.

### Submitting and inspecting jobs

`submit TYPE PAYLOAD` passes the type and payload unchanged to `Engine::submit`.
Use quotes for multi-word payloads, for example `submit PRINT "hello world"`.
An explicit empty payload (`submit PRINT ''`) is valid; omitting it is an error.
Single quotes can contain double quotes and vice versa. Commas and backslashes
are literal bytes. There are no shell escapes, JSON conversion, or submission
options for priority/retries in this command; the existing Engine defaults apply.

Success appears only after submission is accepted and includes the actual ID.
The displayed status is sampled afterward and may already be Running or
Completed. Unknown types and startup/shutdown/storage errors are reported through
the existing runtime error path; errors keep the session alive.

Handlers must be registered before Engine startup. The standalone
`jobq_cli interactive` registers none, so its submissions fail until application
handlers are wired into startup. EMAIL/REPORT/CALCULATE from the one-shot `run`
demonstration are not automatically registered in the interactive Engine.
Applications using the checkout's `jobq_cli_runtime` target can supply its
initialization callback, which receives the session's single Engine before start:

```cpp
#include <iostream>
#include <syncstream>
#include "cli/InteractiveRuntime.h"
#include "cli/JobCommands.h"

int main() {
    jobq::cli::InteractiveRuntime runtime({.workerCount = 1}, [](jobq::Engine& engine) {
        engine.registerHandler("PRINT", [](const jobq::Job& job) {
            std::osyncstream(std::cout) << jobq::cli::summarizeJobText(job.getPayload()) << '\n';
        });
    });
    return runtime.run(std::cin, std::cout, std::cerr);
}
```

The callback should register handlers without starting or stopping the Engine.
It runs once; exceptions prevent the command loop from starting. Captured
application dependencies must outlive worker shutdown. CLI runtime headers and
the CLI target remain checkout-only, separate from installed `JobQ::JobQ`.
An application may set `EngineConfig::storagePath` in the constructor to use the
existing journal/recovery implementation. The standalone interactive invocation
continues to accept no storage/worker flags.

`jobs` lists ID, safely rendered type, current status, priority, and attempts
made. It prints `No jobs available.` for an empty registry, shows one row per ID,
and includes all retained records without pagination or a silent row limit.
Large lists therefore cost memory/time proportional to retained records and
their payloads. `job ID` additionally shows a payload preview and retry policy.
No timestamps, saved last error, or next execution deadline are recorded by
this API; those fields display Unavailable. Pending includes initially queued
jobs and prepared retries. Attempts and retry policy distinguish their history,
while the queue command reports aggregate ready/retry placement.

Types and payload previews quote and escape backslashes, quotes, control bytes,
DEL, and non-ASCII bytes using visible hexadecimal escapes. At most the first
160 source bytes are shown; longer values include a truncation notice and total
byte count. The stored payload is unchanged. `jobs` omits payloads entirely.
Unknown IDs produce a not-found error. Zero, signs, whitespace within the ID,
non-decimal text, overflow, and extra arguments produce usage errors.

Listing/lookup use `Engine::listJobs()` and `Engine::getJob()` value snapshots,
with no direct CLI journal access and no worker or scheduling changes. Each job
snapshot is synchronized; a list can span nearby execution instants. Jobs remain
in the registry for the Engine's lifetime, including after shutdown. Configured
storage is recovered at construction, then the registry reflects live state.
External journal edits are not refreshed during a session. In-memory records
vanish when their Engine is destroyed. Corrupt complete records fail startup;
the existing journal implementation repairs incomplete tails. Listing itself
does no storage I/O and cannot report a fresh journal read error.

### Interactive metrics and bounded monitoring

Inside `jobq_cli interactive`, `metrics` reads the session Engine's existing
`Engine::metrics()` snapshot. No counters are recalculated from command history,
job lists, or the journal, and inspection never resets them. Before an Engine is
initialized, the command reports an error rather than fabricated zero statistics.
After shutdown the retained Engine's cumulative metrics remain readable through
the runtime interface. This is distinct from the one-shot `jobq_cli metrics`
operation described later, which cannot recover another process's memory metrics.

| Field | Actual meaning |
|---|---|
| Submitted jobs | Accepted fresh submissions counted by JobManager after enqueue succeeds. Retries and recovered jobs do not increment this counter. |
| Started executions | Worker attempts counted after a job successfully transitions to Running. |
| Completed executions | Attempts recorded as successfully finished, after the Completed state transition succeeds. |
| Failed executions | Attempts whose processing throws after execution timing starts. Failure reporting precedes the failed-state transition and retry scheduling. |
| Scheduled retries | Successful retry enqueues, including recovered retry scheduling; not additional fresh submissions. |
| Finished executions | Completed executions plus failed executions, as computed by the existing metrics snapshot. |
| Total execution | Sum of recorded successful and failed attempt durations. |
| Average execution | Existing integer-nanosecond total divided by finished executions; zero before any attempt finishes. |
| Metrics elapsed | Steady-clock time since JobMetrics construction, including Engine setup and subsequent idle time. |
| Cumulative throughput | Finished executions divided by metrics elapsed seconds, in executions/s. |

Execution timing begins after the Running transition and includes start-counter
recording, worker diagnostics, handler execution, and the successful Completed
transition (including its persistence). For failure, it ends at exception capture,
before failed-state persistence and retry scheduling. It excludes queue wait and
retry delay, and is broader than handler-only duration. Total durations sum across
workers and can exceed wall time. A retried job can contribute multiple failed or
successful attempts. Counts are neither unique completed jobs nor queue depth.

Each metrics snapshot is internally synchronized, but counter updates for one
job happen in separate operations. A worker can start before submission counting
finishes, and a failure can be visible before its retry counter increments.
Metrics are in memory and reset with a new Engine; recovered journal records do
not restore historical counters. They do not expose worker activity, percentile
latencies, recent rolling windows, or a persisted history. Metrics elapsed time
continues after stop, so cumulative throughput can decline while idle/stopped.

Counters display as integers. Durations use ns, us, ms, or s according to magnitude;
scaled durations and rates show three decimal places. A newly started Engine
shows zero counters/durations and zero finished-attempt throughput. Small positive
rates can round to 0.000 executions/s.

```text
jobq> metrics
jobq> monitor
jobq> monitor 100 3
jobq> metrics
jobq> status
```

`monitor` defaults to an interval of 1000 ms and five samples. Custom arguments
must both be provided: interval 1–10000 ms, sample count 1–60, and
interval × sample count at most 60000 ms. Zero, negative/signed values, fractional
numbers, overflow, unsupported options, and extra/missing arguments are rejected
before monitoring starts. The limit bounds requested waits; OS scheduling,
snapshot retrieval, and output overhead can make wall time longer.

Monitoring prints a separate initial Baseline, then waits before each numbered
sample. Exactly the requested number of samples follow unless shutdown or an
error interrupts the operation. Each sample shows cumulative metrics and an
Interval changes section for submitted/started/completed/failed/retry/finished
counters. Finished execution rate divides the finished-attempt delta by the
actual steady-clock interval between snapshot captures, including output and
sampling overhead. It does not assume the requested wait matches elapsed time,
and it is distinct from cumulative throughput. There is no rolling-window rate.

Counter/time decreases, inconsistent finished totals, or nonpositive measured
intervals yield an Unavailable indication rather than underflow or a negative
rate. The next sample uses the latest baseline and can resume normal reporting.
Normal Engine metrics have no reset command; reset handling protects interval
calculations from an unexpected reset or inconsistent input.

This is a synchronous bounded command: the input loop waits while existing worker
threads continue execution. No extra monitoring threads or collectors are created,
and no Engine or metrics locks are held during waits or terminal formatting.
After the final sample, `Monitoring completed.` is printed and the prompt returns.
If another application thread requests shutdown, monitoring notices the Engine
state at interval boundaries, reports that monitoring stopped, and returns.
It can wait up to one configured interval before noticing a stop. Output failures
abort the command without a completion message. Objects remain owned by the
runtime; callers must finish concurrent monitoring before destroying their owner.
Interactive cancellation, signals, and asynchronous command input remain future
lifecycle work. `exit` entered while monitoring is read after the command returns.

### Input grammar

The parser splits a line into a command name and an ordered argument list.
Whitespace separates tokens, single or double quotes group text, and empty
quoted arguments are preserved. Adjacent quoted/unquoted fragments concatenate
into one argument. Inside a quoted fragment, the other quote character is literal.
Backslashes are always literal, including in Windows paths. Escape sequences,
variable expansion, shell pipelines, and command chaining are unsupported.
Unclosed quotes, non-whitespace control characters, and an empty quoted command name are errors.
Option-shaped tokens such as `--format` are ordinary ordered arguments at this
layer; individual future commands will validate their accepted options.

### Runtime architecture and scope

`InteractiveCommand` parses terminal input without touching the Engine.
`CommandDispatcher` maps registered names to callables and generates help from
that registry. `InteractiveRuntime` owns an Engine, registers the built-in
commands, runs the input loop, reports failures, and coordinates shutdown.
Terminal streams are supplied to `run()` for deterministic testing. Its internal
CLI extension points let future command handlers access that same Engine.
Registry operations are used on the session thread. Runtime snapshots can also
be read concurrently with execution and shutdown. A runtime object
supports one call to `run()` and cannot restart its Engine.

The CLI runtime is built separately from the public library and uses `JobQ::JobQ`.
Its CLI headers are excluded from installation. Existing one-shot handlers retain
their established core integration. Advanced lifecycle/signal handling will
be added incrementally. Their names are not advertised as working session commands.
The one-shot operations below retain their argument-based behavior. In particular,
one-shot `status JOB_ID` inspects a stored job, while session `status` takes no
arguments and inspects the running runtime. Interactive submission uses the
registered handlers and live Engine described above.

---

## 1. Syntax & Global Options

```text
jobq_cli <command> [arguments...] [options...]
```

### Global Flags
- `-h`, `--help`: Displays command usage and available subcommands.

### Exit Codes
- `0`: Successful execution.
- `1`: Command execution, runtime I/O, startup/shutdown error, or job not found.
- `2`: Invalid invocation, command arguments, or options.

### Architecture Note
The existing argument-based operations follow a **one-command-per-process** model.
They operate against the specified `--storage` journal file. In-memory runtime
metrics are collected and reported during the lifetime of the one-shot `run`
command. `jobq_cli interactive` starts the persistent session described above.

---

## 2. Command Index

| Command | Syntax | Description |
|---|---|---|
| [`submit`](#1-submit) | `submit --type TYPE --payload DATA [options]` | Enqueues a new job into the persistent journal. |
| [`list`](#2-list) | `list [--storage PATH]` | Displays all jobs recorded in the journal. |
| [`status`](#3-status) | `status <JOB_ID> [--storage PATH]` | Displays detailed metadata for a specific job. |
| [`run`](#4-run) | `run [--workers N] [--storage PATH] [--format text\|json]` | Starts a worker pool to execute all pending jobs. |
| [`metrics`](#5-metrics) | `metrics [--format text\|json]` | Explains runtime metrics availability or exports in-process metrics. |

---

## 3. Subcommand Details

### 1. `submit`
Submits a new job to storage.

```text
jobq_cli submit --type TYPE --payload PAYLOAD [options]
```

#### Options:
- `--type <TYPE>` *(Required)*: Job category name (e.g. `EMAIL`, `REPORT`, `BACKUP`).
- `--payload <STRING>` *(Required)*: Arbitrary payload string or JSON data.
- `--priority <low|normal|high>` *(Optional, default: `normal`)*: Scheduling priority.
- `--retries <N>` *(Optional, default: `0`)*: Maximum retry attempts on failure.
- `--storage <PATH>` *(Optional, default: `data/jobq.journal`)*: Path to the journal file.

#### Example:
```powershell
.\build\jobq_cli.exe submit --type REPORT --payload "daily-sales" --priority high --retries 3 --storage data/app.journal
```

#### Sample Output:
```text
Job submitted successfully.
ID: 1
Type: REPORT
Priority: HIGH
Maximum retries: 3
Status: PENDING
Storage: data/app.journal
```

---

### 2. `list`
Lists all jobs currently recorded in the journal file with their status and attempt count.

```text
jobq_cli list [--storage PATH]
```

#### Example:
```powershell
.\build\jobq_cli.exe list --storage data/app.journal
```

#### Sample Output:
```text
Listing all jobs from: data/app.journal
ID: 1 | Type: REPORT | Priority: HIGH | Status: PENDING | Attempts: 0
ID: 2 | Type: EMAIL  | Priority: NORMAL | Status: COMPLETED | Attempts: 1
ID: 3 | Type: BACKUP | Priority: LOW    | Status: FAILED    | Attempts: 3
Total jobs: 3
```

---

### 3. `status`
Inspects a single job by its numerical ID.

```text
jobq_cli status <JOB_ID> [--storage PATH]
```

#### Example:
```powershell
.\build\jobq_cli.exe status 1 --storage data/app.journal
```

#### Sample Output:
```text
Job ID: 1
Type: REPORT
Priority: HIGH
Status: PENDING
Attempts made: 0
Max retries: 3
Payload: daily-sales
Storage: data/app.journal
```

If the job does not exist:
```text
Error: Job ID 999 not found in data/app.journal
```

---

### 4. `run`
Starts the worker pool, recovers pending and interrupted jobs from the journal, executes all registered handlers, drains the queue, and outputs execution metrics.

```text
jobq_cli run [--workers COUNT] [--storage PATH] [--format text|json]
```

#### Options:
- `--workers <COUNT>` *(Optional, default: `1`)*: Number of concurrent worker threads.
- `--storage <PATH>` *(Optional, default: `data/jobq.journal`)*: Journal file to recover and process.
- `--format <text|json>` *(Optional, default: `text`)*: Output format for runtime metrics.

#### Example (Formatted Text Output):
```powershell
.\build\jobq_cli.exe run --workers 2 --storage data/app.journal --format text
```

#### Sample Output:
```text
Starting JobQ with 2 workers...
Storage: data/app.journal
[Worker 1] picked Job 1 (REPORT, priority HIGH, attempt 1)
[CLI] Processing REPORT: daily-sales
[Worker 1] Job 1 completed after 1 attempt(s)
[Worker 1] stopped
[Worker 2] stopped
JobQ stopped cleanly.
Total jobs tracked: 1
Completed jobs: 1
Failed jobs: 0
Pending jobs: 0
Running jobs: 0
Cancelled jobs: 0

========== JobQ Runtime Metrics ==========

Job Statistics

  Submitted Jobs:          0
  Started Executions:      1
  Completed Executions:    1
  Failed Executions:       0
  Scheduled Retries:       0
  Finished Executions:     1

Performance Statistics

  Total Execution Time:    5.47 ms
  Average Execution Time:  5.47 ms
  Elapsed Time:            20.69 ms
  Throughput:              48.33 jobs/sec

==========================================
```

#### Example (Structured JSON Output):
```powershell
.\build\jobq_cli.exe run --workers 2 --storage data/app.journal --format json
```

#### Sample Output:
```text
Starting JobQ with 2 workers...
Storage: data/app.journal
[Worker 1] picked Job 1 (REPORT, priority HIGH, attempt 1)
[CLI] Processing REPORT: daily-sales
[Worker 1] Job 1 completed after 1 attempt(s)
[Worker 1] stopped
[Worker 2] stopped
JobQ stopped cleanly.
Total jobs tracked: 1
Completed jobs: 1
Failed jobs: 0
Pending jobs: 0
Running jobs: 0
Cancelled jobs: 0

{
  "schemaVersion": 1,
  "metrics": {
    "submittedJobs": 0,
    "startedExecutions": 1,
    "completedExecutions": 1,
    "failedExecutions": 0,
    "scheduledRetries": 0,
    "finishedExecutions": 1
  },
  "performance": {
    "totalExecutionTimeNs": 5469100,
    "averageExecutionTimeNs": 5469100,
    "elapsedTimeNs": 20692800,
    "throughputJobsPerSecond": 48.32598778
  }
}
```

---

### 5. `metrics`
Explains metrics availability or exports in-process metrics.

```text
jobq_cli metrics [--format text|json]
```

#### Behavior & Output:
Because metrics reside in memory during a worker run, running `metrics` as a standalone separate process prints an explanatory message to `stderr` and exits with code `1`:

```powershell
.\build\jobq_cli.exe metrics --format json
```

```text
Error: Standalone 'metrics' command cannot report metrics across
independent processes because metrics are stored in memory.
Runtime metrics are reported automatically upon completion of 'run'.
```

If an invalid format argument is supplied:
```powershell
.\build\jobq_cli.exe metrics --format invalid
```
```text
Error: Invalid format. Use text or json.
```
