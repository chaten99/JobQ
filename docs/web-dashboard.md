# JobQ web dashboard

The React and TypeScript dashboard is a separate application in `dashboard/`.
Vite builds the browser bundle. React Router handles navigation, Axios reads
JobQ's REST API, and Zustand tracks the event connection and reconciliation.
Tailwind CSS and local shadcn/ui components provide the interface.

## Run locally

Use Node.js 22.12+ or 24.x and npm. Build the optional C++ HTTP server
following the [server guide](http-server.md), then start `jobq_server` on
`127.0.0.1:8080`. In a second terminal:

```powershell
cd dashboard
npm ci
npm run dev
```

Open the local URL printed by Vite. The dev server proxies `/api` and
`/health` to JobQ at port 8080, including the WebSocket upgrade for
`/api/v1/events`. The standalone server starts with no application
handlers. To try actual job activity from a Windows checkout, run
`jobq_rest_test_host.exe` from the server-enabled test build in place of
`jobq_server.exe` and submit an `ECHO` job through the REST API.

## Configuration and deployment

| Variable | Default | Use |
|---|---|---|
| `VITE_JOBQ_API_BASE_URL` | Same-origin paths | Base URL or path for REST requests. |
| `VITE_JOBQ_WS_URL` | Derived from API base and page origin | Full WebSocket URL when event traffic uses a different route. |

Vite reads these as build-time settings. Development overrides can be kept in
`.env.local`. An HTTPS origin derives a `wss:` connection. An alternate origin
needs a deployment that permits browser requests to that origin.

To build and verify the dashboard:

```powershell
npm ci
npm run typecheck
npm test
npm run build
```

Serve `dashboard/dist` through a web server and forward `/health` and `/api`
to JobQ on the same origin. Forward WebSocket upgrades and serve `index.html`
for frontend routes such as `/jobs/123`. The C++ server remains a separate
process and the dashboard build runs independently of CMake.

## Data and live updates

Overview counts come from the retained job list totals, one snapshot per
status. Recent jobs come from the newest page of ascending job IDs. Jobs
supports status and exact type filters, pagination, and linked detail
pages. Payload text is a preview, with encoding and truncation explained
in the detail view. Queue and worker pages show only supported aggregate
fields. Counts may reflect nearby sampling instants.

The browser opens one WebSocket connection and retries after unexpected
disconnects with capped exponential backoff and jitter. Each connection
gets its own sequence tracking. IDs and sequences stay decimal strings.
The dashboard keeps up to 100 events from the current connection and
fetches REST snapshots on initial connection, reconnect, manual refresh,
and after coalescing incoming events. It does not poll on an interval.
Reconnects cannot recover the missed event history. Retry PENDING events
represent retry preparation before queue insertion.

The dashboard is for inspection. It exposes no worker controls, historical
metrics, or authentication. See the [REST API](http-api.md) and
[WebSocket contract](http-events.md) for response semantics.

## Metrics and browser testing

The Metrics page shows the Engine's submissions, execution attempts, and queued
retries alongside cumulative and average execution times. Its lifetime rate
counts both successful and failed attempts. The success share divides successful
by finished attempts once at least one attempt finishes.

The chart derives interval rates from the difference in finished attempts divided
by the change in the Engine's elapsed time. It keeps up to 30 intervals in memory
while the page is open. Backwards counters or elapsed time clear the history.
Equal elapsed times produce no point. Requests run one at a time, ten seconds
after the prior request finishes, while the page is visible. A return to the tab,
manual refresh, or event-stream reconnect requests a fresh snapshot. Failed
refreshes preserve the last successful values and show an error.

From the repository root in PowerShell with MSYS2 UCRT64 on PATH:

```powershell
$env:Path = "C:/msys64/ucrt64/bin;$env:Path"
cmake -S . -B build/e2e -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DJOBQ_BUILD_SERVER=ON -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build/e2e --target jobq_rest_test_host -j 6
$env:JOBQ_E2E_HOST = (Resolve-Path build/e2e/server/jobq_rest_test_host.exe).Path
cd dashboard
npm ci
npx playwright install chromium
npm run e2e
```

Playwright starts its own test host and Vite on OS-assigned loopback ports.
No manually started backend is needed. If startup fails, check that
`JOBQ_E2E_HOST` points to the built executable, UCRT64 runtime DLLs are on PATH,
and endpoint protection permits the test processes to bind loopback ports.
The suite tests actual REST and WebSocket traffic through Vite. For a manual
Vite session the default backend remains port 8080. `JOBQ_PROXY_TARGET` can
override that destination for an isolated workflow.
