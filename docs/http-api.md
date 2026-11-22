# JobQ REST API

Build and run the [optional HTTP server](http-server.md) using Drogon 1.9.14.
The default listener is `127.0.0.1:8080`. Health remains available at
`GET /health/live` and `GET /health/ready`.

| Method and path | Result |
|---|---|
| `GET /api/v1/jobs` | Filtered, paginated job snapshots. |
| `POST /api/v1/jobs` | Submit through the running server-owned Engine. |
| `GET /api/v1/jobs/{id}` | One actual retained job snapshot, or 404. |
| `GET /api/v1/queue` | The single queue's ready/delayed waiting entries and closed flag. |
| `GET /api/v1/workers` | Aggregate worker pool lifecycle and configured count. |
| `GET /api/v1/runtime` | Server and Engine lifecycle, uptime, admission, worker configuration, and JobQ version. |

## Shared conventions

Successful individual resources use `{"data":{...}}`. Lists add a pagination
object: `{"data":[],"pagination":{"limit":50,"offset":0,"total":0}}` for an
empty default collection. Errors use `{"error":{"code":"JOB_NOT_FOUND",
"message":"The requested job does not exist."}}`. Responses have JSON content
type and `Cache-Control: no-store`; parser-generated oversized-body errors also
use JSON. Raw exception details, stack traces, and storage paths are never returned.

Job IDs are positive decimal uint64 values represented as **JSON strings**, so
JavaScript clients can retain them exactly. Lookup rejects zero, signs, whitespace,
non-decimal text, and overflow with 400. Leading decimal zeroes are accepted and
responses use the canonical actual ID. An absent record is 404, not an empty success.
An empty path segment has no individual-job route and can receive framework 404.

## List jobs

```powershell
curl.exe 'http://127.0.0.1:8080/api/v1/jobs'
curl.exe 'http://127.0.0.1:8080/api/v1/jobs?limit=10&offset=0&status=COMPLETED&type=PRINT'
```

| Query | Default and rules |
|---|---|
| `limit` | 50; unsigned decimal integer 1–100. |
| `offset` | 0; unsigned decimal uint64 fitting the platform's size_t. |
| `status` | Optional exact `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, or `CANCELLED`. |
| `type` | Optional exact nonempty type, at most 256 bytes after URL decoding. |

Filters combine with AND. Ordering is ascending actual job ID. `total` counts all
matching snapshots before slicing; offset beyond that total returns an empty
array with the same total. Unknown/repeated/empty query parameters, malformed or
overflowing numbers, zero/oversized limit, and unsupported statuses return 400.
URL-encode type values appropriately. Listing is read-only.

Pagination bounds the response, **not the Engine scan**: Engine::listJobs copies
all retained job values, including full payloads, then the HTTP layer filters and
slices them. Time/memory grow with the registry; no database pagination, cursor,
eviction, or retention policy is implied. Each job snapshot is synchronized;
the full list can span nearby worker execution instants. Requests for subsequent
pages can observe changed statuses or newly submitted jobs.

## Submit a job

Content type must be `application/json` (case-insensitive, optional parameters
such as `charset=utf-8`). A nonempty JSON object must contain type and payload:

```json
{
  "type": "PRINT",
  "payload": "hello world",
  "priority": "NORMAL",
  "retry": {
    "max_retries": 1,
    "initial_delay_ms": 100,
    "max_delay_ms": 5000,
    "multiplier": 2
  }
}
```

`type` must be a nonempty string of at most 256 bytes. `payload` must be a string;
an explicit empty string is valid. Numbers, objects, arrays, null, and booleans
are not coerced into payload text. Whitespace, quotes, backslashes, commas, and
decoded string bytes are passed to Engine unchanged. JSON syntax must be valid;
duplicate keys, comments, trailing commas, extra documents, and nesting beyond
64 levels are rejected. Unknown top-level or retry fields are rejected.

`priority` is optional and must be `LOW`, `NORMAL`, or `HIGH`. Default is NORMAL.
`retry` is optional; its fields independently override the actual JobOptions policy:

| Retry field | Default | API bounds |
|---|---|---|
| `max_retries` | 0 | JSON integer 0–10, after the initial attempt. |
| `initial_delay_ms` | 100 | JSON integer 0–60000. |
| `max_delay_ms` | 5000 | JSON integer 0–60000. |
| `multiplier` | 2 | Finite JSON number 1–10. |

Booleans, numeric strings, fractional integer fields, negative values, and
overflow are invalid. Retry delays follow existing exponential backoff/capping:
initial delay above max delay is capped, and zero delay/cap gives immediate retry.
This is existing retry support, not initial scheduling or a requested execution
deadline. No unknown scheduling/options fields are silently ignored.

Submit success is **201 Created**, only after Engine accepts the job. The
`Location` header points to `/api/v1/jobs/` followed by the actual returned ID.
`data` contains the job snapshot sampled after acceptance; the status can already
be RUNNING or COMPLETED. Inspect the returned ID; sample IDs are not guaranteed.
There is no idempotency key: retrying a timed-out POST can create another job if
the first submission succeeded before the connection failed.

Save the request above as `request.json`, then submit and inspect the returned ID:

```powershell
curl.exe -i -X POST http://127.0.0.1:8080/api/v1/jobs -H 'Content-Type: application/json' --data-binary '@request.json'
```

The standalone server registers **no production handlers**. That PRINT request
returns 422 until a host registers a real PRINT handler using the existing
`ServerRuntime(config, [](jobq::Engine& engine) { ... })` initializer before start.
Call `engine.registerHandler` there; do not start or stop Engine in the initializer.
Test hosts register their own handlers and do not add fake production services.
Engine validates the actual registry; REST does not maintain a second registry.

## Job records and payload policy

List entries, creation responses, and individual lookup use the same fields:
`id`, `type`, `type_truncated`, `status`, `priority`, `attempts_made`, `retry`,
`payload_preview`, `payload_preview_encoding`, `payload_bytes`, and
`payload_truncated`. Retry includes the four actual policy fields above. Attempts
count starts, so RUNNING can have one attempt and a prepared retry can be PENDING
with prior attempts. FAILED is job state, not a cumulative failed-attempt counter.

Payload previews include at most 160 original bytes. Printable ASCII is preserved;
backslashes, controls, DEL, and non-ASCII bytes become visible `\xHH` sequences.
The encoding field is `escaped_bytes`; JSON escaping additionally protects the
JSON string. Size reports full original byte count and truncation is explicit.
Stored payload is not modified and there is no full-payload retrieval option.
Type output is limited to its first 256 bytes with a truncation flag for records
created by hosts with longer types. API submissions themselves enforce 256 bytes.
No timestamps, scheduled deadlines, saved execution errors, or mutable internals
are fabricated. Previewing is a size policy, not secret redaction: applications
must treat all job data as potentially sensitive and add access controls before
external exposure. Individual lookup accepts no query options.

## Queue snapshot

```powershell
curl.exe http://127.0.0.1:8080/api/v1/queue
```

`data` has `ready` and `delayed` integer counts and a `closed` boolean, taken
directly from the existing synchronized Engine/queue snapshot. Ready entries are
waiting, not executing. Delayed entries are in the retry heap; due retries stay
there until normal retrieval promotes them. Inspection does not promote/remove
anything. No capacity, active-worker count, submission total, or historical
execution count is presented as depth. There is one queue, not a queue collection.
The operation accepts no query parameters. Once the server is unavailable or
shutdown is requested it returns 503; an already admitted snapshot can reflect
a nearby lifecycle transition, including queue closure.

## Errors and lifecycle

| HTTP status | Machine code | Meaning |
|---|---|---|
| 400 | `INVALID_JSON` | Empty/malformed/deep/duplicate-key JSON. |
| 400 | `INVALID_REQUEST` | Invalid fields, options, filters, pagination, or ID syntax. |
| 404 | `JOB_NOT_FOUND` | Valid individual ID absent from registry. |
| 404 | `RESOURCE_NOT_FOUND` | No such HTTP route. |
| 405 | `METHOD_NOT_ALLOWED` | Unsupported method, with Allow header. |
| 413 | `PAYLOAD_TOO_LARGE` | Existing configured framework body limit exceeded. |
| 415 | `UNSUPPORTED_MEDIA_TYPE` | POST content type is not application/json. |
| 422 | `UNKNOWN_JOB_TYPE` | Engine rejected an unregistered job type. |
| 500 | `INTERNAL_ERROR` | Unexpected or storage/submission failure; generic message only. |
| 503 | `ENGINE_UNAVAILABLE` | Engine not ready or shutdown requested. |
| Other framework errors | `HTTP_ERROR` | Generic HTTP-layer failure. |

Runtime availability is checked before reading/submitting data. A small context
admission mutex linearizes Engine submission against requestStop; no subsequent
REST submit passes after that gate closes. Component locks are released before
JSON construction and callback delivery. Shared context captures keep Engine
alive throughout route callbacks; no request constructs a processing component.
Read-only Engine APIs remain safe if shutdown starts after the availability check.
Listener shutdown may close a connection rather than return 503; Drogon does not
guarantee every in-flight response drains. Engine still drains accepted work and
joins workers. See [server lifecycle](http-server.md#ownership-and-lifecycle).

The pinned Drogon parser normally sends a bare oversized-body response before
routes. A small source-checked FetchContent patch routes those parser errors
through its configured error handler, preserving JSON 413 without weakening the
body limit. Route validation does not echo parser diagnostics or exception text.

Server storage remains in memory by default and jobs do not survive restart.
Checkout hosts may set `ServerConfig::storagePath` before constructing the runtime
to use existing Engine journal/recovery; no command-line storage flag is added.
Listing and lookup read live/recovered registry values without direct file access.
They do not reread external journal edits, and history metrics are not recovered.
Storage writes can fail submission (500), while recovery failure prevents startup.

The CLI and server are separate processes with separate in-memory Engines. There
is no distributed/shared backend, authentication, authorization, CORS, or rate
limiting in this increment. Bind remains local by default. Do not expose it as a
production service without later security work. Verification here is Windows
MinGW UCRT64; POSIX/MSVC/macOS are not claimed tested.

## Worker and runtime inspection

Both endpoints are read-only GET requests with no query parameters. Responses use
the same JSON envelope and no-store header as the other versioned endpoints.

```powershell
Invoke-RestMethod http://127.0.0.1:8080/api/v1/workers
Invoke-RestMethod http://127.0.0.1:8080/api/v1/runtime
```

The worker response contains `data.state` and `data.configured_workers`.
The lifecycle comes from the pool snapshot. The state mapping prioritizes
`stopped`, then `stopping`, then `running` if the pool has started, and
otherwise `created`. In normal operation a ready server reports `running`.
The count is the configured number of workers. Active and idle counts, individual worker
identities, and worker health are unavailable. There are no worker control
or scaling routes. A zero-worker configuration is rejected at startup.

The runtime response contains these fields:

| Field under `data` | Meaning |
|---|---|
| `server.state` | `running` when the verified listener is ready and shutdown has not been requested at sampling time. |
| `server.uptime_seconds` | Whole seconds since the listener passed its readiness probe, measured using a steady clock. |
| `engine.state` | The sampled Engine lifecycle. A successful response requires `running`. |
| `engine.accepting_jobs` | Whether server admission is open at sampling time. Individual submissions can still fail validation, handler lookup, or storage. |
| `engine.configured_workers` | Configured count from the Engine's worker pool snapshot. |
| `version.jobq` | Version from JobQ's public version header. |

Unsupported query parameters return 400 `INVALID_REQUEST`. Before readiness,
after shutdown is requested, and after the Engine stops, requests reaching a
handler return 503 `ENGINE_UNAVAILABLE`. After listener closure, clients may
instead see a closed or refused connection. Unexpected exceptions return a
generic 500 `INTERNAL_ERROR`. No bind or storage configuration is exposed.

The server samples its existing Engine and WorkerPool. Component values and
separate HTTP responses may represent nearby instants. A successful response does
not promise that the runtime will still be ready when a later request arrives.

## Verification

Run `ctest --test-dir build/http-debug -R 'jobq_(rest_|server_)' --output-on-failure`.
Route tests cover unavailable/shutdown states, strict validation, errors, real
jobs, previews, pagination/filtering, concurrent inspection, storage rollback,
and recovery. Real HTTP tests exercise workers, URL parameters, method/status
mapping, pending/running/delayed/failed/retried states, and active-work shutdown.
The Windows subprocess host test captures a returned ID, processes and inspects
it over HTTP, then verifies shutdown and port release. It uses bounded timeouts
and reaps its child on failure. Full Debug/Release suites run sequentially.
