# JobQ HTTP server foundation

`jobq_server` is a separate host for the reusable JobQ Engine. This increment
provides health endpoints plus the [job and queue REST API](http-api.md).
The REST API also includes read-only worker pool and runtime snapshots. [WebSocket events](http-events.md) report live job status changes. Authentication and a dashboard remain future increments.

## Framework and build

The server uses [Drogon 1.9.14](https://github.com/drogonframework/drogon/releases/tag/v1.9.14),
an asynchronous C++ HTTP framework with JSON, routing, and WebSocket support.
The pinned tag resolves to `3aae769cc405cf5abb9d477675c6430924f02c7c` and brings
Trantor at `78e480f5e138acdc05f76c55a2716718cd846364` (1.5.29). It compiles with
JobQ's C++20/UCRT64 toolchain. Framework platform support is not evidence of local
verification on other compilers; this task executes Windows MinGW UCRT64 only.

The optional server defaults OFF, preserving library/CLI builds without downloads.
When ON, CMake FetchContent clones `v1.9.14` and its pinned Trantor submodule into
the build directory. No framework sources or generated dependency files are
committed. FetchContent requires Git/network on the first configure; populated
build directories can be reused offline. There is one acquisition strategy,
rather than parallel installed-Drogon and FetchContent modes.

The server path requires CMake 3.28+ for dependency exclusion semantics. The
server-disabled library retains its existing CMake 3.20 minimum. Drogon and
Trantor build as static libraries; ORM/database drivers, tools/examples/docs/tests,
Brotli, YAML configuration, c-ares, and TLS providers are disabled. Framework
source still compiles its common ORM value/stub sources; it enables no database
backends. JSON and compression depend on toolchain-installed JsonCpp and zlib.
They remain private to the server and outside JobQ's installed package exports.

On MSYS2 UCRT64, install the matching prerequisites without mixing MSVC binaries:

```text
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-jsoncpp mingw-w64-ucrt-x86_64-zlib git
```

This verification uses GCC 16.2.0, JsonCpp 1.9.8, and zlib 1.3.2. JsonCpp/zlib
are system prerequisites rather than vendored pins; distribution builds must
record and supply compatible versions. With Windows CMake and UCRT64 on PATH:

```powershell
cmake -S . -B build/http-debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DJOBQ_BUILD_SERVER=ON -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build/http-debug -j 6
ctest --test-dir build/http-debug --output-on-failure
.\build\http-debug\server\jobq_server.exe --help
.\build\http-debug\server\jobq_server.exe
```

Repeat with a separate `build/http-release` and Release configuration; run full
test suites sequentially because legacy core tests share some temporary paths.
`-DBUILD_TESTING=OFF` excludes server tests along with other JobQ tests. Setting
`-DJOBQ_BUILD_SERVER=OFF` avoids framework discovery/build entirely.

The server executable is built in the server binary directory and is not part of
the existing library installation/export. Static Drogon/Trantor do not imply a
fully static executable: UCRT64 JsonCpp/zlib and compiler runtime DLLs must be
available on PATH or packaged alongside it. No deployment installer is added.
No MSVC/Linux/macOS build or signal test is claimed executed here. POSIX code is
conditional; Unix toolchains need matching JsonCpp/zlib development packages.

## Configuration

Command-line arguments are the sole configuration source. Options require a
separate value, may appear once, and reject malformed/signed/overflowing numbers.
`--help`/`-h` must appear alone. Unknown arguments are errors.

| Option | Default | Allowed values |
|---|---|---|
| `--bind` | `127.0.0.1` | Numeric IPv4 or IPv6, without URL/brackets/hostname. |
| `--port` | `8080` | 0–65535; 0 requests an OS-assigned ephemeral listener. |
| `--http-threads` | `2` | 1–64 framework request I/O threads, separate from JobQ workers. |
| `--engine-workers` | `4` | 1–256 Engine workers. |
| `--max-body-bytes` | `1048576` | 1–16777216 bytes; total and memory body limits. |
| `--max-connections` | `1024` | 1–65536; global and per-IP connection limit. |

Connections have a 30-second idle timeout. Framework logging uses WARN, the
Server header is empty, and static file extensions are disabled. Requests are
unencrypted development HTTP; TLS/auth/CORS/rate limiting are future design work.
External binding requires an explicit address such as `--bind 0.0.0.0` or `::`;
the default never exposes all interfaces. HTTP thread and body limits are applied
through Drogon's real configuration APIs.

```powershell
.\build\http-debug\server\jobq_server.exe --bind 127.0.0.1 --port 8081 --http-threads 2 --engine-workers 4 --max-body-bytes 65536 --max-connections 128
curl.exe http://127.0.0.1:8081/health/live
curl.exe http://127.0.0.1:8081/health/ready
```

Port 0 prints the actual selected port in the readiness message. Successful
liveness returns JSON `{"status":"ok"}` with HTTP 200 and JSON content type.
Readiness returns `{"status":"ready"}`/200 only when Engine is running, the
listener has passed its real HTTP probe, the framework is running, and shutdown
has not been requested. Otherwise an accessible readiness handler returns
`{"status":"not_ready"}`/503. Once listeners stop, connection failure is normal;
there is no promise that a shutting-down process remains available to return 503.
Health responses expose no journal paths, environment values, or secrets.

## Ownership and lifecycle

The runtime owns one shared context containing one Engine and synchronized
listener/shutdown flags. Health handlers capture that context, so its lifetime
extends through framework-managed requests. No request creates an Engine, queue,
manager, worker pool, or independent metrics collector. Future API routes will
receive this same context. Drogon's singleton framework supports one run per
process; the runtime rejects repeated runs or a second host in that process.

`jobq_cli` and `jobq_server` are different OS processes. Each owns its own Engine;
they do not share in-memory jobs or workers. The library is common implementation,
not a distributed backend. The server currently uses in-memory Engine storage and
registers no application handlers. The REST submission route rejects unknown types.
Checkout hosts can set ServerConfig::storagePath for existing Engine recovery;
no journal command-line flag is added.

Startup validates arguments, checks listener bind availability, initializes and
starts Engine, registers routes/limits/listener, then runs Drogon. A bounded
loopback GET probe confirms real HTTP response before readiness is published;
beginning-advice registration alone is not treated as listening success. A
five-second readiness deadline and one-second request timeout bound that probe.
The real startup code configures framework signal handling off, replacing its
unsafe C++ signal callback with a minimal lock-free notification. A main-loop
timer polls notifications/host requests every 25 ms and invokes framework quit
from normal code. HTTP processing stops through Drogon and its I/O threads join
before Engine stop drains accepted work and joins JobQ workers.

Windows Ctrl+C/Ctrl+Break and C-runtime SIGINT/SIGTERM notify graceful shutdown;
POSIX SIGINT/SIGTERM use scoped sigaction. Registrations are restored afterward.
Native Windows smoke tests deliver Ctrl+Break. OS force kills, Windows console
close/logoff deadlines, and nonreturning application handlers cannot guarantee
graceful cleanup. No detached threads or CLI signal session are reused by the
server. Framework shutdown and repeated `requestStop()` calls converge on the
same stop path. Health requests perform no job mutation/network wait under locks.

Invalid configuration returns 2, runtime/startup/cleanup failures return 1, and
successful normal/framework/signal shutdown returns 0. Initialization errors
stop acquired workers; common unavailable/occupied listeners fail cleanly. The
framework's checked-in Trantor bind code calls exit(1), so a small idempotent
FetchContent patch changes only that bind failure to a thrown exception, enabling
Engine cleanup even if a port is claimed after validation. The patch verifies
the expected pinned source text and fails configure if it no longer matches.
Trantor's other catastrophic low-level failures, including a post-bind listen
failure/resource exhaustion, can still terminate the process; no stronger
exception-safety guarantee for all third-party internals is claimed.

Shutdown uses Drogon's quit semantics, not a new request-draining guarantee:
listeners stop and active connections may be closed; handlers' shared context
remains alive until framework cleanup. Engine graceful stop retains its existing
policy for accepted jobs/retries. There is no new HTTP drain timeout or restart
feature. Timer registration is invalidated after run completes. See the
[public Engine guide](api-usage.md) for core lifecycle semantics.

## Tests

With the server enabled, focused tests are available via
`ctest --test-dir build/http-debug -R jobq_server --output-on-failure`.
Configuration/context tests cover defaults, numeric/address validation, readiness
503 before startup and during shutdown, actual Engine readiness, partial startup
cleanup, and single-use behavior. Repeated host-stop requests are tested through
the running framework. Process tests cover invalid configuration/unavailable
addresses. Windows-only tests exercise a real subprocess on an ephemeral port,
JSON/status/content type, readiness, absence of future APIs, console shutdown,
worker joining, released port, and a post-validation bind failure with cleanup.
Timeouts terminate/reap child processes on failure. HTTP tests use only loopback,
not public internet. Build-time dependency fetching requires network separately.

Library/CLI/demo targets and installed JobQ package exports remain unchanged.
The optional dependency's tests and tools are not included in JobQ's CTest suite.

The REST increment also adds response/validation/snapshot serialization helpers,
a shutdown admission gate, and route/HTTP/subprocess workflow tests. A second
small source-checked framework patch sends pre-routing HTTP parser failures
through Drogon's configured JSON error handler; body limits remain enforced.
