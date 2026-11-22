# Real-time WebSocket events

Connect to `ws://127.0.0.1:8080/api/v1/events` after
`GET /health/ready` returns 200. Use a WebSocket upgrade with no query
parameters. The server keeps one subscription per live connection. All
subscribers receive the same live event stream. No subscription messages
are needed. Sending a text or binary command closes that connection.
Standard WebSocket control frames are handled by the framework.

```javascript
const socket = new WebSocket("ws://127.0.0.1:8080/api/v1/events");

socket.addEventListener("message", ({ data }) => {
  const event = JSON.parse(data);
  if (event.event === "job.status_changed") {
    console.log(event.sequence, event.data.job_id, event.data.status);
  }
});
```

Send a job through the [REST API](http-api.md) from a host with a registered
application handler. The checkout's `jobq_rest_test_host` registers `ECHO`
for local testing. The standalone production executable leaves handler
registration to its host.

```json
{
  "event": "job.status_changed",
  "sequence": "1",
  "data": {
    "job_id": "123",
    "previous_status": "PENDING",
    "status": "RUNNING",
    "attempts_made": 1
  }
}
```

The publisher assigns increasing server-local sequences to committed
transitions. Both sequence and job ID are decimal
strings. Compare sequences numerically with BigInt if needed. The
per-job transitions are ordered, and events for different jobs can
interleave. The counter starts over with a new server process.

Initial creation and submission do not produce events. Execution emits
`PENDING → RUNNING`, then `RUNNING → COMPLETED` or `RUNNING → FAILED`.
A retry emits `FAILED → PENDING` after the state change commits and before
queue insertion, so it reports retry preparation. A later `RUNNING` event
confirms that execution actually started. Other committed transitions,
such as cancellation if triggered by the Engine, use the same schema.
Events contain no payload, timestamps, or worker identity.

The stream is best-effort and in memory. There is no replay on reconnect
and no durable event history. The server retains up to 256 recent
transitions for currently connected clients. It sends at most 16 events
per batch, then waits for the matching WebSocket pong. Standard browser
clients reply automatically. A client closes if it falls behind the retained
buffer or fails to acknowledge a batch within five seconds. One slow client
does not hold up the others.

Before readiness and after shutdown is requested, upgrade attempts may be
closed immediately or refused. Shutdown closes existing subscriptions
while the Engine drains accepted jobs. Drain-time transitions may be
absent. Use `GET /api/v1/jobs/{id}` or a paginated job list to reconcile
current state after reconnecting. Runtime and worker snapshots remain
available through their REST endpoints while the server is ready.

The default listener is local. This increment adds no authentication,
authorization, TLS, or distributed broker.

## C++ status subscriptions

The additive overload `Engine(EngineConfig, JobStatusListener)` accepts a
callback taking `JobStatusChange` by value. It runs inline on the thread
committing each transition, after state and any persistence write have
succeeded. Recovered transitions during construction can also produce
notifications. Callbacks for different jobs can run concurrently.
Keep callbacks short, thread safe, and nonblocking. They run with the
job state lock held to preserve per-job ordering, so callbacks must not
call back into JobQ or perform network I/O. Exceptions from the listener
are contained by the Engine. Omitting the listener preserves the existing
Engine behavior.
