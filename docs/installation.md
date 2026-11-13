# Installation and Setup Guide

This guide describes how to set up the development environment, configure, build, and test **JobQ** on your system.

---

## 1. System Requirements

JobQ is implemented in modern C++20 and uses CMake as its build generation system.

### Verified Environment
- **Operating System:** Windows 10 / 11 (64-bit)
- **Compiler:** GCC 13+ / 14+ via **MSYS2 MinGW UCRT64**
- **Build System:** CMake 3.20 or newer
- **Shell:** Windows PowerShell or Command Prompt
- **Threads:** Native C++20 threading (`std::thread`, `std::mutex`, `std::condition_variable`)

> [!NOTE]
> The primary development and CI verification platform is Windows with MSYS2 MinGW UCRT64.
> JobQ is written to standard C++20 and standard CMake, so it should compile on Linux (GCC 11+, Clang 13+) and macOS, but Windows UCRT64 is the officially tested environment.

---

## 2. Toolchain Setup (Windows / MSYS2 UCRT64)

### Step 1: Install MSYS2
1. Download and install MSYS2 from [https://www.msys2.org/](https://www.msys2.org/).
2. Launch the **MSYS2 UCRT64** terminal from the Start Menu.

### Step 2: Install Compiler and Build Tools
Run the following package manager command inside the MSYS2 UCRT64 terminal:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
                   mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja \
                   git
```

### Step 3: Add UCRT64 to Windows PATH
Add the MSYS2 UCRT64 binary path to your Windows user or system `PATH` environment variable:

```text
C:\msys64\ucrt64\bin
```

### Step 4: Verify Tools in PowerShell
Open a new **PowerShell** window and confirm that the toolchain is accessible:

```powershell
gcc --version
g++ --version
cmake --version
git --version
```

Expected output indicates GCC 13+ or 14+ with C++20 support and CMake 3.20+.

---

## 3. Cloning the Repository

Clone the JobQ repository to your local drive:

```powershell
git clone https://github.com/chaten99/JobQ.git
cd JobQ
```

---

## 4. Configuring and Building

JobQ uses standard out-of-source CMake builds.

### Step 1: Configure CMake
Generate the build files into the `build/` directory:

```powershell
cmake -S . -B build
```

CMake will automatically:
- Detect the C++20 compiler.
- Locate the platform threading library (`Threads::Threads`).
- Configure main application targets, CLI binaries, stress test targets, and CTest integration.

### Step 2: Compile the Project
Compile all targets using CMake:

```powershell
cmake --build build
```

This compiles:
- `build/jobq.exe` â€” Standalone demo executable.
- `build/jobq_cli.exe` â€” Main JobQ Command-Line Interface.
- `build/jobq_stress_test.exe` â€” Concurrency stress test harness.
- All 27 CTest test binaries.

To build in parallel using all available CPU cores:

```powershell
cmake --build build --parallel
```

---

## 5. Running Tests

JobQ includes an extensive automated test suite integrated with **CTest**.

Run all 27 test suites from the root directory:

```powershell
ctest --test-dir build --output-on-failure
```

Expected output:
```text
Test project D:/JobQ/build
      Start  1: jobq_manager_lifecycle
 1/27 Test  #1: jobq_manager_lifecycle .................   Passed
      Start  2: jobq_priority_scheduling
 2/27 Test  #2: jobq_priority_scheduling ...............   Passed
      ...
      Start 27: jobq_queue_stress
27/27 Test #27: jobq_queue_stress ......................   Passed

100% tests passed out of 27
```

To run a single specific test by name (e.g. `jobq_metrics_json`):

```powershell
ctest --test-dir build -R jobq_metrics_json --output-on-failure
```

---

## 6. Running the Binaries

### Interactive Demo
```powershell
.\build\jobq.exe
```

### CLI Application
```powershell
.\build\jobq_cli.exe --help
```

Submit and run a sample job:
```powershell
# Submit a job
.\build\jobq_cli.exe submit --type REPORT --payload "daily-summary" --storage data/demo.journal

# Run workers to process jobs and output metrics
.\build\jobq_cli.exe run --workers 2 --storage data/demo.journal --format json
```

---

## 7. Troubleshooting Common Issues

### Issue 1: Compiler does not support C++20
- **Symptom:** CMake error stating `The CXX compiler does not support C++20`.
- **Solution:** Ensure you are using the **UCRT64** environment of MSYS2 (`C:\msys64\ucrt64\bin`), not the legacy MSYS or MinGW32 toolchain. Verify that `g++ --version` outputs version 11 or higher.

### Issue 2: Missing Threads Library
- **Symptom:** CMake fails with `Could NOT find Threads (missing: Threads_FOUND)`.
- **Solution:** MinGW GCC includes native POSIX/Win32 threading headers. Ensure the complete GCC toolchain is installed (`mingw-w64-ucrt-x86_64-gcc`).

### Issue 3: In-use File Locks on Windows
- **Symptom:** Linker errors such as `cannot open output file ... Permission denied`.
- **Solution:** Ensure no running instance of `jobq.exe` or `jobq_cli.exe` is currently open in another terminal. Stop running background processes before rebuilding.
