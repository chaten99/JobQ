# Optional Redis connectivity

Stage 14.2 adds an internal transport for future shared-backend operations.
The existing Engine uses its local queue and storage. This module is not an
implementation of `jobq::backend::Backend` and does not write jobs to Redis,
allocate IDs, claim work, or manage distributed leases.

## Build

The transport uses Boost.Redis for RESP3 and Boost.Asio for connection
management. Boost.Redis itself first shipped with Boost 1.84. This module
requires Boost 1.92 or later for its connection setup, response tree and
per-request cancellation APIs. It requires Redis 6 or later with RESP3 and
OpenSSL. JobQ uses C++20. The Windows verification toolchain is MinGW UCRT64
GCC 16.2.0 with the MSYS2 Boost 1.92.0 and OpenSSL 3.6.4 packages.

In an MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-boost mingw-w64-ucrt-x86_64-openssl
```

In PowerShell with UCRT64 tools on PATH:

```powershell
cmake -S . -B build/redis-debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DJOBQ_BUILD_REDIS_BACKEND=ON -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build/redis-debug -j 6
ctest --test-dir build/redis-debug -R '^jobq_redis_' --output-on-failure
```

`JOBQ_BUILD_REDIS_BACKEND` defaults OFF. With it off, CMake does no Redis
dependency discovery. With it on, `jobq_redis_transport` links Boost and
OpenSSL privately and is available to checkout targets. The installed core
package does not include the transport or these dependencies.

## Connection configuration

| Field | Default and validation |
| --- | --- |
| Host | `127.0.0.1`, configured as a DNS name or address without a scheme, brackets, credentials, or path. |
| Port | 6379. Range 1 through 65535. |
| Username and password | Optional. Supplying a username requires a nonempty password. Password alone authenticates the Redis default user. Values are kept out of exceptions and logs. |
| Database | 0. Nonnegative integer. The server may reject unsupported database indices. |
| Client name | `jobq`. Printable ASCII without spaces, up to 128 characters. |
| Connection timeout | 1000 ms for each lookup and connect stage. Positive, up to five minutes. |
| Command timeout | 1000 ms per command. Positive, up to five minutes. |
| Reconnect delay | 250 ms. Zero disables reconnect. Maximum five minutes. |
| Namespace | `default`. One to 64 ASCII letters, digits, underscores or hyphens. |
| TLS | Disabled. Requests to enable TLS are rejected until certificate and hostname verification are implemented. |

Use the transport only on a trusted local or protected link. Supply passwords
from an external secret source and avoid shell arguments, connection URLs,
tracked configuration and logs for real credentials. RESP3 commands are
serialized as separate binary-safe arguments.

## Runtime and errors

`start()` launches one Asio thread and one Boost.Redis `async_run()`. It
can be called once. Independent synchronous callers post asynchronous requests
to that thread and wait without holding the lifecycle lock. A real
`PING` returning `PONG` marks health as responsive. Health reflects the
last observed result, with idle, connecting, responsive, unavailable, and
stopped states. It does not attest to Redis persistence or replication.

Transport error categories distinguish connection failure, authentication,
permission, timeout, command errors, protocol errors, shutdown, and
unexpected response shapes. Only redacted categories leave the client.
A disconnected request fails rather than looking like an empty queue.
The connection can reconnect automatically, but failed or unacknowledged
requests are not replayed by the transport. Timeout means Redis may have
executed a command without the client seeing its result. Shutdown rejects
new work, cancels connection activity, drains callbacks and joins the thread.

## Key layout and scripts

Keys use `jobq:{namespace}:v1:<category>`. The namespace forms the Redis
Cluster hash tag, so future multi-key operations can target one slot. This
design does not implement cluster routing. Categories reserved for later
operations include `ids`, `records`, `ready:high`, `ready:normal`,
`ready:low`, `delayed`, `workers`, `claims`, `generations`, and `schema`.
A namespaced `probe` key is available for the isolated integration test.

`loadScript()` issues `SCRIPT LOAD` and returns an opaque script handle with
its Redis SHA. `eval()` sends `EVALSHA` with separate keys and arguments.
Keys must belong to the transport namespace. An explicit `NOSCRIPT` response
proves the script did not run. The transport reloads that source, checks the
SHA and retries once. Timeouts and connection loss do not trigger a retry.
The script deadline applies separately to each command in that workflow.
Scripts are trusted adapter code and must not emit the Redis missing-script
diagnostic themselves.

## Real-server tests

Use a dedicated local test server with permission for PING, HELLO, SELECT
when configured, CLIENT LIST, SET, GET, DEL, BLPOP, SCRIPT LOAD and EVALSHA.
For example, in an Ubuntu WSL2 distribution, install Redis
and start a foreground test instance on a free port:

```sh
sudo apt update
sudo apt install redis-server
redis-server --bind 127.0.0.1 --port 6380 --save "" --appendonly no
```

Then from Windows PowerShell, assuming WSL localhost forwarding is active:

```powershell
$env:JOBQ_REDIS_TEST_HOST = "127.0.0.1"
$env:JOBQ_REDIS_TEST_PORT = "6380"
ctest --test-dir build/redis-debug -R '^jobq_redis_' --output-on-failure
```

Optional settings are `JOBQ_REDIS_TEST_USERNAME`,
`JOBQ_REDIS_TEST_PASSWORD` and `JOBQ_REDIS_TEST_DATABASE`. The suite is
reported skipped if the host setting is unset. It writes one unique
namespaced probe key and deletes only that key. It does not flush databases.

To verify reconnect and missing-script recovery, run `jobq_redis_test --recovery`
against a disposable test server. Stop it after the executable prints
`READY`, then restart the same endpoint with an empty script cache after
`OUTAGE`. The executable verifies a new PONG and reloads its harmless script.
Expect `RECOVERED`. Ordinary CTest does not stop a configured Redis instance.
