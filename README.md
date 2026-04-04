# Phanes

[![CMake Build](https://github.com/jnyfah/phanes/actions/workflows/build.yml/badge.svg)](https://github.com/jnyfah/phanes/actions/workflows/build.yml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=jnyfah_phanes&metric=alert_status&token=70f44ef44415031cc2a1d2e532ec7a1d88553720)](https://sonarcloud.io/summary/new_code?id=jnyfah_phanes)

Phanes is a fast, multithreaded command-line tool for analyzing filesystem structure. It scans directories in parallel and produces detailed reports on file sizes, types, modification times, symlinks, errors, and directory metrics, all from a single pass.

---

## Features

- Parallel directory scanning via a lock-free work-stealing thread pool
- Concurrent action dispatch: multiple report types computed simultaneously
- benchmark ?????

---

## Installation

### Prerequisites

- [CMake](https://cmake.org/) 4.0 or later
- A C++23-capable compiler:
  - **Linux / macOS** -- Clang 18+ or GCC 13+
  - **Windows** -- MSVC 19.38+ (Visual Studio 2022 17.8+)
- [Ninja](https://ninja-build.org/) (for the default presets on Linux/macOS)

### Build

```bash
git clone https://github.com/jnyfah/phanes.git
cd phanes
```

**Linux / macOS**
```bash
cmake --preset ninja-release
cmake --build --preset ninja-release
```

**Windows (Visual Studio)**
```bash
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The binary is placed in `build/<preset>/phanes` (or `phanes.exe` on Windows).

---

## Usage

```
phanes <path> [flags...]
```

At least one flag is required. Multiple flags can be combined freely — each report is computed concurrently and printed in the order the flags were given.

### Flags

| Flag | Argument | Description |
|---|---|---|
| `--summary` | — | Overall scan summary: total files, size, depth, duration |
| `--largest-files` | `<N>` | Top N files by size |
| `--largest-dirs` | `<N>` | Top N directories by recursive size |
| `--recent` | `<N>s/m/h/d` | Files modified within the last N seconds, minutes, hours, or days |
| `--extensions` | — | Per-extension file count and total size |
| `--empty-dirs` | — | Directories containing no files or subdirectories |
| `--symlinks` | — | All symbolic links found during the scan |
| `--errors` | — | Filesystem entries that could not be accessed |
| `--metrics` | — | Per-directory depth, recursive size, and file count |
| `--stats` | — | Aggregate statistics: deepest path, largest directory, averages |

### Examples

```bash
# Quick overview of a directory
phanes /home/user --summary

# Find the 10 largest files and see extension breakdown
phanes /home/user --largest-files 10 --extensions

# See what changed in the last hour and check for errors
phanes /var/log --recent 1h --errors

# Full report
phanes /srv --summary --largest-files 20 --largest-dirs 10 --extensions --metrics --stats
```

---

## License

This project is open source. See [LICENSE](LICENSE) for details.
