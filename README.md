# antares

`antares` is an **experimental command-line tool** built to explore and practice **modern C++ (C++23)** features in a realistic, systems-style project.

This project is **not** meant to be a polished end-user utility.
It is a **learning and experimentation playground** for:

- modern C++ design principles
- expressive data modeling
- concurrency and async abstractions
- clean separation of concerns
- performance-aware code

The focus is on **how** the code is written, not just **what** it does.

---

## Project Goals

This project exists to experiment with and gain hands-on experience in:

- **Modern C++ style**
  - RAII and value semantics
  - move semantics and ownership
  - expressive APIs and readability

- **C++17 / C++20 / C++23 features**
  - `std::filesystem`
  - `std::optional`, `std::variant`, `std::expected`
  - `std::chrono`
  - ranges and views
  - concepts
  - modules
  - `std::format`

- **Concurrency & async**
  - `std::jthread`
  - `std::stop_token`
  - atomics
  - coroutines (generators, async tasks)
  - experimentation with **senders / receivers** (where supported)

- **Architecture**
  - layered design (data -> analysis -> orchestration -> presentation)
  - testable, composable components
  - minimal global state

---

## What `antares` Does

`antares` analyzes a directory by:

1. **Building an in-memory model** of the directory tree
2. **Running analytical reports** on that model
3. **Presenting results** in human-readable (or machine-readable) form

The filesystem traversal and the analysis are **explicitly separated**.

---

## Supported Reports

- Global summary (file count, directory count, total size)
- File type / extension distribution
- Top-N largest files
- Recently modified files
- Directory size ranking
- Directory structure statistics (depth, density)
- Empty / trivial directories
- Error and anomaly report (permission issues, broken symlinks, etc.)

Reports are independent and can be enabled selectively via CLI flags.

---

## Command-Line Usage

```bash
diranalyze <path> [options]
```

CLI Flags
Core

--help — show help

--version — show version

--summary — global summary (default if no report flags are provided)

Reports

--by-type — file extension distribution

--top N — top-N largest files

--recent <duration> — recently modified files (7d, 12h, etc.)

--dirs — directory size ranking

--structure — directory structure statistics

--empty — empty / trivial directories

--errors — error and anomaly report

Output

--format <text|json> — output format (default: text)

--output <file> — write output to file

--quiet — suppress non-essential output

Performance & Behavior

--threads N — number of worker threads

--progress — show progress updates

--cancel-on-error — stop on first fatal error

Requirements
Language & Compiler

C++23

A compiler with good C++23 support:

GCC 13+

Clang 16+

MSVC (latest)

Build System

CMake 3.26+

Ninja (recommended, but optional)

Platform

Linux

macOS

Windows (tested via MSVC / clang-cl)

Building the Project
Configure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release


(You may use another generator if preferred.)

Build
cmake --build build

Installing (Optional)

To install the executable locally:

cmake --install build


This typically installs diranalyze into /usr/local/bin (platform-dependent).

Running

From the build directory:

./build/diranalyze <path> [options]


Or, if installed:

diranalyze <path> [options]

Project Structure (High-Level)
src/
  core/        # data models, concepts, utilities
  fs/          # filesystem traversal & data collection
  analysis/    # pure analysis functions (reports)
  async/       # concurrency & coroutine experiments
  output/      # formatting & presentation
  app/         # CLI parsing and orchestration
