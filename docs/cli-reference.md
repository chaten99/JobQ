# CLI Reference Guide

`jobq_cli` is the command-line interface for the JobQ engine. It allows users to submit tasks, inspect job registries, execute worker pools from persistent storage, and export runtime metrics.

---

## 1. Syntax & Global Options

```text
jobq_cli <command> [arguments...] [options...]
```

### Global Flags
- `-h`, `--help`: Displays command usage and available subcommands.

### Exit Codes
- `0`: Successful execution.
- `1`: Invalid command arguments, unknown options, job not found, or command execution error.

### Architecture Note
`jobq_cli` follows a **one-command-per-process** model. Commands operate against the specified `--storage` journal file. In-memory runtime metrics are collected and reported during the lifetime of the `run` command.

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
