# CMake build-tree package

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

This build-tree export uses `export(TARGETS ...)`. CMake's `export(EXPORT ...)`
form requires an installation export set established by `install(TARGETS ...)`.
Installation rules are outside this change, so both library targets are listed
explicitly in the build-tree export. `configure_package_config_file` and
`write_basic_package_version_file` generate the configuration and version files.
The configuration helper's `INSTALL_DESTINATION` argument supplies package
layout metadata without adding installation rules.

Exported build paths refer to the producer's original source tree for headers
and build tree for libraries. Keep both trees available. Moving those trees or
copying the generated files to an installed prefix is unsupported. The
`INSTALL_INTERFACE` include path prepares the usage requirements for later
installation work. This change provides build-tree consumption only.

The separate project under `tests/package_consumer` compiles a single application
source, links to `JobQ::JobQ`, runs an Engine, and checks job status and metrics.
It requests C++17 and uses `std::span` to verify that the library raises the
effective requirement to C++20. It also discovers the package twice to verify
repeatable configuration. Run its CTest suite after building it.
