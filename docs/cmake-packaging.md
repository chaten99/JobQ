# CMake packages and installation

JobQ provides package `JobQ` at development version `0.1.0`. Configure and build
the producer with the same compiler, architecture, and compatible build
configuration as the consuming application.

```powershell
cmake -S . -B build/package-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/package-release --target jobq_api
cmake -S tests/package_consumer -B build/consumer-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DJobQ_DIR=D:/JobQ/build/package-release/cmake/JobQ
cmake --build build/consumer-release
ctest --test-dir build/consumer-release --output-on-failure
```

Adapt `D:/JobQ` to the checkout location. An application with its own source files
can use this CMake setup.

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApplication LANGUAGES CXX)
find_package(JobQ 0.1 CONFIG REQUIRED)
add_executable(my_application main.cpp)
target_link_libraries(my_application PRIVATE JobQ::JobQ)
```

Set `JobQ_DIR` to the generated package directory, or add that directory to
`CMAKE_PREFIX_PATH`. Build the producer libraries before linking the consumer.
The consumer has its own build and discovers JobQ through `find_package`.

| Target | Purpose |
|---|---|
| `JobQ::JobQ` | Public Engine API, backed by the existing `jobq_api` static library. |
| `JobQ::Core` | Lower-level processing engine, backed by `jobq_core`. |
| `JobQ::API` | Compatibility alias for `JobQ::JobQ`. |

The same names work when adding the source checkout through `add_subdirectory`.
Existing `jobq_api` and `jobq_core` target names remain available in that build.
The package exports the two existing libraries using their `EXPORT_NAME`
properties. The API links publicly to the core, which publishes its public
include directory, C++20 feature requirement, and `Threads::Threads` dependency.
The package locates Threads before loading the exported targets. Consumers
receive those requirements when linking to the namespaced target.

The generated files are all under `<build-directory>/cmake/JobQ`.

| File | Purpose |
|---|---|
| `JobQConfig.cmake` | Finds dependencies, loads targets, and provides the compatibility alias. |
| `JobQConfigVersion.cmake` | Checks requested version and architecture compatibility. |
| `JobQTargets.cmake` | Imports library locations and public usage requirements from this build tree. |

`cmake/JobQConfig.cmake.in` is the tracked configuration template. Version
compatibility uses `SameMinorVersion`, so requests within the same major/minor
line are compatible when the requested patch is available. `EXACT` requests
require a matching version. The initial `0.1.0` version identifies development
metadata and establishes no stable major-release or ABI guarantee.

The build-tree export uses `export(EXPORT JobQTargets ...)` and the installed
export uses `install(EXPORT JobQTargets ...)`. Both share the export set established
by the two library installation rules. `configure_package_config_file` generates
build-tree and installation configurations from the same template.
`write_basic_package_version_file` supplies their shared version metadata.

Exported build paths refer to the producer's original source tree for headers
and build tree for libraries. Keep both trees available. Moving those trees or
copying the build-tree package files to an installed prefix is unsupported.
Use the installation workflow below to create a relocatable package.

The separate project under `tests/package_consumer` compiles a single application
source, links to `JobQ::JobQ`, runs an Engine, and checks job status and metrics.
It requests C++17 and uses `std::span` to verify that the library raises the
effective requirement to C++20. It also discovers the package twice to verify
repeatable configuration. Run its CTest suite after building it.

## Installed package

Build and install using a prefix selected for your application.

```powershell
cmake -S . -B build/release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
cmake --install build/release --prefix C:/temp/jobq-prefix
cmake -S examples/installed-consumer -B C:/temp/jobq-consumer -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/temp/jobq-prefix
cmake --build C:/temp/jobq-consumer
ctest --test-dir C:/temp/jobq-consumer --output-on-failure
```

Use unique temporary paths or a dedicated prefix. With a multi-configuration
generator, select the same configuration for the library and consumer and pass
`--config Release` to build/install and `-C Release` to CTest.

`GNUInstallDirs` controls destinations. The libraries go under
`CMAKE_INSTALL_LIBDIR`, headers under `CMAKE_INSTALL_INCLUDEDIR`, and package
files under `${CMAKE_INSTALL_LIBDIR}/cmake/JobQ`. These usually resolve to
`lib`, `include`, and `lib/cmake/JobQ` within the selected prefix. The installed
package includes `JobQConfig.cmake`, `JobQConfigVersion.cmake`,
`JobQTargets.cmake`, and a configuration-specific targets file. Applications
and CLI executables are built as before; installation delivers the libraries.

The public headers preserve their existing directory layout. `jobq/Engine.h`,
`jobq/EngineConfig.h`, and `jobq/JobOptions.h` provide the high-level API. The
`job`, `queue`, `executor`, `worker`, `manager`, `storage`, and public `metrics`
headers support `JobQ::Core` consumers and the API's existing value types.
CLI headers and `metrics/MeasuredLock.h` are excluded from installation.

An independent application uses the same `find_package` and target-linking
commands shown above. Linking `JobQ::JobQ` supplies `JobQ::Core` and Threads
transitively, along with public includes and C++20. Its source can use the API
as follows.

```cpp
#include "jobq/Engine.h"

int main() {
    jobq::Engine engine({.workerCount = 2});
    engine.registerHandler("TASK", [](const jobq::Job&) {});
    engine.start();
    const auto id = engine.submit("TASK", "payload");
    engine.stop();
    return engine.getStatus(id) == jobq::JobStatus::Completed ? 0 : 1;
}
```

The installed consumer in `examples/installed-consumer` uses a latch to observe
handler completion, then drains and verifies job status and metrics. Its own
CMake project discovers the package and compiles only its application source.
It can be copied to another directory and built independently.

Installed metadata resolves libraries and includes relative to its prefix.
Copy the complete installation directory to another location, configure a fresh
consumer with the new `CMAKE_PREFIX_PATH`, and rebuild it. Use relative
`CMAKE_INSTALL_LIBDIR` and `CMAKE_INSTALL_INCLUDEDIR` values to preserve this
relocatability. Absolute custom installation destinations are outside the
relocatable layout. Match the compiler, architecture, runtime, and configuration
of the producer libraries. Static libraries remain the supported library type.
Package-manager integrations are future work.
