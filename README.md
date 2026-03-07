# phanes

[![CMake Build](https://github.com/jnyfah/phanes/actions/workflows/build.yml/badge.svg)](https://github.com/jnyfah/phanes/actions/workflows/build.yml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=jnyfah_phanes&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=jnyfah_phanes)

A tiny CLI filesystem analyzer — and a hands-on playground for learning modern C++23 features.

The primary goal of phanes is not the tool itself but the journey: every module, algorithm, and utility in this codebase is deliberately chosen to exercise a C++23 (or recent C++20) language/library feature. The analyzer is just the domain that makes it concrete.

---

## C++23 Features Explored

| Feature | Where |
|---|---|
| **Named modules** (`.ixx`, `export module`, `import`) | Every source file |
| `std::ranges::fold_left` | `analyzer.cpp` — depth/file sums |
| `std::ranges::partial_sort` + `iota` | `analyzer.cpp` — top-N files/dirs |
| `std::ranges::max_element`, `sort`, `transform` | `analyzer.cpp` |
| `std::span` | `main.cpp` argv handling, `parse()` signature |
| `std::variant` + `std::visit` | `executor.ixx` — action dispatch |
| `std::format` | `view.cpp` — formatted table output |
| `std::from_chars` | `parser.cpp` — zero-allocation number parsing |
| `std::chrono::clock_cast` | `builder.cpp` — `file_time_type` → `sys_time` |
| `std::chrono::days` | `parser.cpp` — `d` unit in `--recent` |
| Structured bindings | Throughout (`auto [ptr, ec]`, `auto [count, total]`) |
| Transparent hash (`is_transparent`) | `analyzer.cpp` — heterogeneous map lookup |
| Lazy memoization with `std::optional` | `executor.ixx` — metrics/empty-dir caching |

> Coroutines are on the roadmap — the builder's iterative DFS walk is a natural fit for a coroutine-based generator.

---

## What It Does

`phanes` walks a directory tree and produces reports based on the flags you pass:

```
phanes <path> [flags...]
```

| Flag | Description |
|---|---|
| `--summary` | Overall scan summary (size, depth, counts, duration) |
| `--largest-files <N>` | Top N files by size |
| `--largest-dirs <N>` | Top N directories by recursive size |
| `--recent <N><unit>` | Files modified within the last N seconds/minutes/hours/days |
| `--extensions` | Extension breakdown by count and total size |
| `--empty-dirs` | Directories with no files or subdirectories |
| `--symlinks` | All symbolic links found |
| `--errors` | Filesystem errors encountered during scan |
| `--metrics` | Per-directory depth and recursive size/file-count table |
| `--stats` | Aggregate directory statistics (max depth, averages, etc.) |

Multiple flags can be combined in a single invocation:

```bash
phanes /home --summary --largest-files 10 --extensions
phanes /var/log --recent 1h --errors
phanes /usr --largest-dirs 5 --stats
```

The `--recent` flag accepts a number followed by a single unit character:

| Suffix | Unit |
|---|---|
| `s` | seconds |
| `m` | minutes |
| `h` | hours |
| `d` | days |

Example: `--recent 24h` lists files modified in the last 24 hours.

---

## Architecture

```
phanes/
└── src/
    ├── core/
    │   └── core.ixx          # shared data types: FileNode, DirectoryNode, DirectoryTree, ErrorRecord
    ├── builder/
    │   ├── builder.ixx        # exports build_tree()
    │   └── builder.cpp        # iterative DFS scan using std::filesystem
    ├── analyzer/
    │   ├── analyzer.ixx       # exports all compute_* functions
    │   └── analyzer.cpp       # pure analysis — no I/O, only ranges algorithms
    ├── parser/
    │   ├── parser.ixx         # flag table, action variant types, ParseResult
    │   └── parser.cpp         # flag dispatch, from_chars parsing
    ├── view/
    │   ├── view.ixx           # exports all print_* functions
    │   ├── view.cpp           # formatted terminal output
    │   └── executor.ixx       # Executor struct — std::visit dispatch over actions
    └── main.cpp               # wires parser → builder → executor
```

The pipeline is strictly linear and stateless between stages:

```
argv → parse() → build_tree() → std::visit(Executor, actions)
```

`Executor` lazily computes `DirectoryMetrics` and empty-directory lists the first time they are needed, so flags that don't require them pay no cost.

---

## Requirements

| Tool | Minimum version |
|---|---|
| CMake | 4.0 |
| C++ compiler | Full C++23 module support |
| Clang | 18+ recommended (tested with clang-18) |
| GCC | 14+ (modules support is still maturing) |

Module support in compilers is still evolving. Clang 18 with CMake 4.0's native module scanning (`CMAKE_CXX_SCAN_FOR_MODULES ON`) is the most reliable combination right now.

---

## Build

```bash
# Configure (out-of-source build)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# The binary ends up at
./build/phanes
```

To use a specific compiler:

```bash
cmake -B build -DCMAKE_CXX_COMPILER=clang++-18
cmake --build build
```

For a debug build with full symbols:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

---

## Run

```bash
# Basic summary
./build/phanes /path/to/dir --summary

# Top 5 largest files and extension breakdown
./build/phanes /path/to/dir --largest-files 5 --extensions

# Files modified in the last 2 hours, plus any errors
./build/phanes /path/to/dir --recent 2h --errors

# Full picture
./build/phanes /path/to/dir --summary --largest-files 10 --largest-dirs 5 \
  --extensions --stats --metrics
```

Running with no flags (or only a path) prints the help text.

---

## License

See [LICENSE](LICENSE).
