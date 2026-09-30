# JobQ

JobQ is a C++20-based background job processing system that queues tasks and executes them concurrently using a worker pool.

## V1 Goals

- Job creation and status tracking
- Thread-safe job queue
- Worker pool using `std::thread`
- Concurrent job execution
- Clean worker lifecycle

## Planned

- Priority jobs
- Retry & backoff
- Persistent jobs & recovery
- Graceful shutdown
- CLI
- Testing & benchmarking
- Optional TCP client/server

## Tech Stack

- C++20
- CMake
- C++ Standard Library
- Git

## Build & Run

### Configure

```bash
cmake -S . -B build
```

### Build

```bash
cmake --build build
```

### Run 
```bash
.\build\jobq.exe   
```

## Project Structure

```text
JobQ/
├── include/
├── src/
├── tests/
├── examples/
├── CMakeLists.txt
└── README.md
```

**Status:** In Development