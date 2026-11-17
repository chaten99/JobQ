# Library versioning and compatibility

JobQ currently identifies itself as development version `0.1.0`. The root CMake
`project(JobQ VERSION ...)` declaration is the authoritative source. CMake
generates `jobq/Version.h`, package configuration, and package version metadata
from that declaration. Both library targets carry the same `VERSION` property.
The libraries remain static, so a shared-object `SOVERSION` is not assigned.

## Version information in C++

Link to `JobQ::JobQ` or `JobQ::Core` and include the generated public header.
It is available from source-tree builds, build-tree packages, and installations.

```cpp
#include <iostream>
#include "jobq/Version.h"

int main() {
    constexpr auto major = jobq::version::major;
    constexpr auto minor = jobq::version::minor;
    constexpr auto patch = jobq::version::patch;
    std::cout << jobq::version::string << '\n';
    return major == 0 && minor == 1 && patch == 0 ? 0 : 1;
}
```

The numeric values are `inline constexpr unsigned` and `string` is an
`inline constexpr std::string_view`. They describe the headers used to compile
the application. Rebuild applications when changing the installed library and
headers. This interface provides compile-time metadata, with no runtime binary
version negotiation.

## Release numbering

Versions follow `MAJOR.MINOR.PATCH`. For a stable major release, incompatible
public API changes require a new major version, backward-compatible functionality
increments the minor version, and backward-compatible corrections increment the
patch version. Introducing version metadata does not establish production
readiness or a stable release.

During `0.x` development, minor releases may change the public source API.
Patch releases within one `0.MINOR` line are intended to preserve source
compatibility while correcting behavior. Backward-compatible additions may also
be introduced in a new development minor release. Consumers should review the
documented changes and rebuild and test when adopting a new minor line. A future
stable major release will require an explicit compatibility review and update
to the package compatibility mode.

Breaking changes should be identified in release notes and migration guidance,
including affected symbols and replacement usage. Future deprecations should
be documented with their replacement and planned removal release. Removal from
a stable line requires a major release, while removal during pre-1.0 development
requires a minor release. This policy introduces no deprecation macros or
guaranteed support period.

## Supported source interfaces

The high-level API consists of `jobq/Engine.h`, `jobq/EngineConfig.h`,
`jobq/JobOptions.h`, and generated `jobq/Version.h`. The supported namespace is
`jobq`, including Engine, EngineConfig, JobOptions, JobId, JobHandler, and
`jobq::version`. Job, JobPriority, JobStatus, RetryPolicy, and JobMetricsSnapshot
are existing public value and handler types used by this interface.

`JobQ::Core` also exposes the installed lower-level headers under `job`, `queue`,
`executor`, `worker`, `manager`, `storage`, and public `metrics`. Their callable
interfaces and documented behavior form the lower-level source API. This
includes job scheduling, executor registration, worker/pool control, job manager
lookup and submission, storage interfaces and record serialization, metrics
snapshots/JSON, latency samples, and execution/contention observation.

Private members, Engine's Impl, storage layout, helper types, and synchronization
implementation are outside the compatibility boundary. `PrimedSharedMutex` is
visible in the manager header as a platform workaround, but is an implementation
helper rather than a supported consumer extension point. The `MeasuredLock.h`
helper and CLI headers are excluded from installation. Repository presence alone
does not establish a supported interface. Internal changes must preserve the
documented public behavior and existing regression tests within the same patch
line. The existing journal format has its own validation/version marker and is
not replaced by the library version.

## CMake package compatibility

The package name is `JobQ`. `JobQ::JobQ` supplies the public API, `JobQ::Core`
supplies the lower-level library, and `JobQ::API` remains a compatibility alias.
The package exposes `JobQ_VERSION`, `JobQ_VERSION_MAJOR`, `JobQ_VERSION_MINOR`,
and `JobQ_VERSION_PATCH` from the authoritative project version.

Both build-tree and installed packages share `SameMinorVersion` rules. A
versioned request requires the same major/minor line and an available version
at least as new as the requested patch. An exact request requires that version.
Thus the current package accepts `0.1`, `0.1.0`, and `0.1.0 EXACT`, and rejects
`0.1.1`, `0.0.0`, `0.2.0`, and `1.0.0`. Unversioned discovery continues to work.

```cmake
find_package(JobQ 0.1 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE JobQ::JobQ)
```

This is a package-selection rule. It cannot verify an application's assumptions
or compiled binary compatibility. Installed package metadata and the generated
header retain the same version when the complete prefix is relocated.

## Binary compatibility

JobQ provides no stable ABI guarantee. Static library consumers must use
compatible compiler, architecture, standard library, runtime, and build settings.
Rebuild and test applications when upgrading. Public class layout, inline code,
templates, standard-library types, and toolchain changes can affect binary
compatibility even when source usage remains valid. Target VERSION metadata
does not certify ABI stability. Long-term support and package-manager integration
are not established by this policy.

## Verification

The version test compiles static assertions for each public value against the
producer's CMake version. The separate project in `tests/version_consumer`
asserts the same header values against the discovered package metadata and runs
using the exported public target. `tests/cmake/package_version_test.cmake`
configures independent consumers for compatible and rejected version requests
and builds/runs the exact-version consumer. The script accepts a package
directory so the same checks can run against build-tree, installed, and relocated
packages.
