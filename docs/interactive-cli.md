# Interactive JobQ CLI

JobQ provides an installed reusable C++ library, a checkout-only interactive CLI,
and the separate `jobq` demonstration executable. Start here for the persistent
developer workflow; the [CLI reference](cli-reference.md) contains detailed field,
grammar, signal, and one-shot command definitions.

## Build and launch

Use CMake 3.20+ and a C++20 compiler with threads. Windows development verification
uses MSYS2 MinGW UCRT64 GCC 16.2.0; keep its bin directory on PATH. See
[installation](installation.md) for setup.

```powershell
cmake -S . -B build/debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j 6
ctest --test-dir build/debug --output-on-failure
.\build\debug\jobq_cli.exe interactive
```

Use a separate `build/release` with `-DCMAKE_BUILD_TYPE=Release` for Release.
Run complete configuration suites sequentially: legacy core tests still use
shared temporary paths across processes. New workflow journals use unique paths.
On POSIX, use the generator/toolchain appropriate to the host and omit `.exe`.
Those commands require local verification; this change was executed on Windows.

The standalone executable accepts `interactive`, or `interactive --help` without
starting workers. No interactive storage/worker options exist. Launching without
a command retains the missing-command error. `jobq` runs the separate demonstration.

```text
help
status
workers
queue
jobs
metrics
monitor 100 3
status
exit
```

This session works without handlers: jobs are initially empty and counters zero.
Actual timings and diagnostics vary. To submit useful work, register application
handlers as described below. No fabricated IDs or timestamps are needed.

## Commands

| Syntax | Behavior and validation |
|---|---|
| `help` | Lists registered commands only. |
| `help <command>` | Usage for exactly one nonempty registered topic. |
| `status` | Runtime/Engine state, configured workers, submitted jobs, uptime; no arguments. |
| `workers` | Pool state/configured count; active/idle and individual details unavailable; no arguments. |
| `queue` | Open/closed state, ready/retry waiting entries, historical attempt counters; no arguments. |
| `submit <type> <payload>` | Exactly two arguments, nonempty registered type; explicit empty payload allowed. Uses default JobOptions. |
| `jobs` | All retained live/recovered records, ascending actual ID, empty-state message; no arguments. |
| `job <job-id>` | One positive decimal uint64 ID; unknown ID reports not found. |
| `metrics` | Existing cumulative metrics snapshot; no arguments. |
| `monitor [<interval-ms> <sample-count>]` | No arguments or exactly two positive unsigned integers, within bounds below. |
| `exit` | Requests graceful shutdown; no arguments. |
| `quit` | Alias for exit; no arguments. |

Unknown commands, missing/extra arguments, malformed quotes, unknown types/IDs,
and invalid monitor/help arguments report errors and leave the session usable.
Commands are case-sensitive. Whitespace splits tokens; single/double quotes group
payload text and preserve internal spaces. `submit PRINT ''` passes an empty
string, `submit PRINT 'hello world'` passes two words as one payload. Backslashes
and commas are literal. Shell escapes, variables, pipelines, and JSON conversion
are absent. Use the alternate quote delimiter for a literal quote character.

## Register real handlers

The standalone interactive entry point registers no application handlers. Its
submission command correctly rejects unregistered types. EMAIL/REPORT in tests
are test handlers, not production email/report services. The one-shot run demo's
handlers are not shared with a separate interactive Engine.

An application in the checkout can link `jobq_cli_runtime` and supply the existing
initialization callback. The callback receives the session Engine before startup;
register handlers without starting or stopping it. This complete host can be
built independently; the workflow fixture uses its own controlled test handlers:

```cpp
#include <iostream>
#include <syncstream>
#include "cli/InteractiveRuntime.h"
#include "cli/JobCommands.h"

int main() {
    jobq::cli::InteractiveRuntime runtime({.workerCount = 2}, [](jobq::Engine& engine) {
        engine.registerHandler("PRINT", [](const jobq::Job& job) {
            std::osyncstream(std::cout) << jobq::cli::summarizeJobText(job.getPayload()) << '\n';
        });
    });
    return runtime.runTerminal(std::cout, std::cerr);
}
```

Save as `host.cpp` alongside this CMake project, replacing the checkout path:

```cmake
cmake_minimum_required(VERSION 3.20)
project(JobQHost LANGUAGES CXX)
add_subdirectory(path/to/JobQ jobq-build EXCLUDE_FROM_ALL)
add_executable(jobq_host host.cpp)
target_link_libraries(jobq_host PRIVATE jobq_cli_runtime)
```

Then run `submit PRINT 'hello world'`, `jobs`, and `job` followed by the actual
returned ID. Success appears only after Engine acceptance. Status can already be
Running or Completed when displayed. Payload/type previews escape unsafe bytes
and show at most 160 source bytes with explicit truncation; stored data is intact.
Records expose priority, attempts, retry policy, status, and payload. Timestamps,
scheduled deadline, and last error text remain unavailable. No pagination/eviction
exists; full listing copies retained payloads and grows with the registry.

CLI headers/target are checkout-only. Installed consumers use the public
[Engine API](api-usage.md) and [CMake package](cmake-packaging.md), not CLI internals.
Keep captured dependencies alive until worker shutdown completes.

## Ownership and monitoring

The runtime owns one Engine, manager, queue, worker pool, handler registry, and
metrics collector for the entire session. Parsing is independent of execution;
the dispatcher registers handlers and generates help. Commands retrieve safe
value snapshots, release locks, and render output. Workers run independently.
Job lists sample records at nearby instants, rather than one global instant.

Metrics count fresh accepted submissions and execution attempts. Started,
completed, failed, scheduled-retry, and finished counters retain the existing
instrumentation semantics: finished equals completed plus failed attempts.
Retries are not new submissions; one job can contribute several attempts.
Total execution duration sums recorded attempt time across workers; average is
total divided by finished attempts. Timing includes instrumentation/diagnostics
and successful completion persistence, excludes queue/retry waits, and is broader
than handler-only time. Cumulative throughput divides finished attempts by time
since collector construction, including idle/setup time. Metrics are not journaled.

`monitor` prints a baseline, then five samples after one-second waits by default.
`monitor 100 3` takes three samples after 100 ms requested waits. Bounds are
1–10000 ms, 1–60 samples, and at most 60000 ms of requested waits. Actual elapsed
time includes scheduling/output overhead. Interval changes subtract consecutive
counters; interval rate divides finished-attempt delta by measured steady-clock
seconds. Counter/time decreases or inconsistent data produce Unavailable for that
interval and establish a new baseline. Zero execution history displays zeros.
No rolling windows, percentiles, persisted history, or active-worker metrics are
invented. See [metrics semantics](cli-reference.md#interactive-metrics-and-bounded-monitoring).

Monitoring is synchronous and finite, with no extra monitoring thread. Workers
continue processing. It returns to the prompt after completion. A shutdown request
interrupts its wait, skips later snapshots, and suppresses successful completion.
Ordinary typed input waits until monitoring returns.

## Shutdown and storage

Exit, quit, EOF, host requests, failures, and supported signals converge on one
coordinator. States progress through Starting, Running, ShutdownRequested,
Stopping, and Stopped, or Failed on errors. Admission closes after a request;
Engine stop drains accepted active/ready work and scheduled retries, then joins
workers. Repeated requests do not create another shutdown path. Handlers that
never return and long retry delays can prolong draining indefinitely.

Windows Ctrl+C/Ctrl+Break and POSIX SIGINT/SIGTERM record minimal notifications;
normal code cancels native idle/partial input and monitoring. Signals are polled
at up to 25 ms intervals; OS scheduling and drain time can add delay. Scoped
registrations restore previous host dispositions. Windows Ctrl+Z/Enter and POSIX
Ctrl+D are EOF. Programmatic Windows `raise(SIGTERM)` follows CRT rules, including
one-shot disposition behavior; native repeated control events stay graceful.
Forced kills and Windows close/logoff deadlines cannot guarantee draining.
Injected `run(input, output, errors)` installs no global handlers and requires
host unblocking for arbitrary blocked streams. `runTerminal` uses native input.

Exit codes are 0 for normal/EOF/host completion, 130 for observed SIGINT, 143 for
SIGTERM, 149 for Windows Ctrl+Break, 1 for startup/I/O/cleanup failure, and 2 for
invalid process invocation. Failures take precedence over graceful signal status.
Full details are in the [shutdown reference](cli-reference.md#shutdown-coordination-and-signals).

The standalone session is in memory: jobs disappear with its Engine. A host may
set `EngineConfig::storagePath` before construction for the existing journal and
recovery path. Recovery restores latest job records and reconciles interrupted
attempts/retries; it does not restore historical metrics. Incomplete tails are
repaired, malformed complete records fail startup. External file edits are not
refreshed during the session. No cross-process coordination, compaction, default
persistence, or fsync durability is added. See [persistence](persistence-and-recovery.md).

## Verification and platform evidence

Run `ctest --test-dir build/debug -R 'jobq_(cli_|interactive_)' --output-on-failure`
for CLI coverage and run the full suite for library/package regressions. The new
`jobq_cli_end_to_end` test drives all commands with actual IDs, two workers,
running/waiting jobs, failure/retry success, monitoring, metrics, cleanup, and
in-memory/persistent recovery. The executable session test covers errors and final
unterminated input; subprocess signal tests have bounded cleanup. Existing tests
cover reset/rate fixtures, corruption, output failure, and partial startup.

This task verifies Windows MinGW UCRT64 GCC 16.2.0 Debug/Release, including native
Windows console signal subprocesses. POSIX tests are conditional and unexecuted
here; MSVC/macOS/Linux are not claimed verified. No GitHub Actions workflow exists
in this checkout; no unverified CI matrix is introduced. See [testing](testing.md)
for the test groups and sequential configuration workflow.
