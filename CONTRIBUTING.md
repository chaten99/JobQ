# Contributing to JobQ

Thank you for your interest in contributing to **JobQ**. This guide explains the development setup, coding standards, testing requirements, and branch workflow.

---

## 1. Development Prerequisites

To contribute code to JobQ, ensure your system meets the following requirements:
- **Compiler:** Modern C++20 compliant compiler (GCC 13+ via MSYS2 MinGW UCRT64 on Windows, or GCC 11+ / Clang 13+ on Linux).
- **Build System:** CMake 3.20 or newer.
- **Git:** Git 2.30+.
- **CLI Tools (Optional):** GitHub CLI (`gh`).

Refer to the [Installation & Setup Guide](docs/installation.md) for full toolchain configuration steps.

---

## 2. Branch & Git Workflow

JobQ follows a feature-branch workflow:

1. **Check out the latest `main` branch:**
   ```powershell
   git checkout main
   git pull origin main
   ```

2. **Create a new feature or documentation branch:**
   ```powershell
   git checkout -b feat/your-feature-name
   # or
   git checkout -b docs/your-topic-name
   ```

3. **Make atomic, focused changes:**
   - Keep commits small, self-contained, and logically coherent.
   - Do not bundle unrelated refactorings or formatting changes with feature implementations.

4. **Follow Conventional Commits:**
   Use standard commit prefixes:
   - `feat:` A new feature or capability.
   - `fix:` A bug fix.
   - `test:` Adding or updating tests.
   - `docs:` Documentation additions or updates.
   - `refactor:` Code restructuring without functional changes.

---

## 3. Coding Standards & Conventions

### Language & Standard
- Write standard **C++20**.
- **No external third-party libraries** for core features (e.g., no Boost, no third-party JSON parsers). Rely strictly on the modern C++ standard library.

### Naming Conventions
- **Types & Classes:** PascalCase (e.g. `JobManager`, `JobQueue`, `JobPriority`).
- **Functions & Methods:** camelCase (e.g. `getAttemptsMade()`, `pushRetry()`).
- **Member Variables:** camelCase with a trailing underscore (e.g. `status_`, `queue_`, `mutex_`).
- **Constants & Enums:** PascalCase for enum values (e.g. `JobStatus::Running`, `JobPriority::High`).
- **Namespaces:** lowercase (e.g. `namespace jobq`, `namespace jobq::cli`).

### Concurrency & Thread Safety
- Protect shared mutable state with standard synchronization primitives (`std::mutex`, `std::condition_variable`, `std::shared_mutex`).
- Always use RAII lock wrappers (`std::lock_guard`, `std::unique_lock`, `std::shared_lock`). Never call raw `.lock()` / `.unlock()`.
- Never hold mutexes across long-running I/O operations or arbitrary third-party callbacks whenever possible. Use immutable snapshots (`JobSnapshot`).

### Formatting & Header Guards
- Use **4 spaces** for indentation. No tabs.
- Use `#pragma once` at the top of all header files.
- Keep header includes ordered: standard library headers first, followed by internal project headers.

---

## 4. Verification & Testing Requirements

Before pushing your branch or submitting a Pull Request, verify that:

1. **The project builds cleanly with zero warnings or errors:**
   ```powershell
   cmake -S . -B build
   cmake --build build
   ```

2. **All 24 CTest test suites pass:**
   ```powershell
   ctest --test-dir build --output-on-failure
   ```
   **100% test passing is strictly required.** No regressions are permitted.

3. **New features include tests:**
   - If adding a new feature or bugfix, add a corresponding automated test in `tests/` and register it with `add_test()` in `CMakeLists.txt`.

4. **Documentation is updated:**
   - Update relevant Markdown files in `docs/` and `README.md` if any public API, CLI command, or architectural behavior changes.

---

## 5. Submitting a Pull Request

1. Push your branch to GitHub:
   ```powershell
   git push origin feat/your-feature-name
   ```
2. Open a Pull Request against the `main` branch:
   ```powershell
   gh pr create --base main --head feat/your-feature-name --title "feat: description" --body "Summary of changes"
   ```
3. A project maintainer will review your Pull Request before merging.
