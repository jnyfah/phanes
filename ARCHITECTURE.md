# Architecture

Phanes scans a directory tree and answers questions about it — total size, largest files and folders, recent changes, extensions, empty dirs, symlinks, errors, and byte-for-byte duplicates. It does this in a single parallel pass: walk the filesystemonce into an in-memory snapshot, then compute every requested report against that snapshot at once.


## Codemap

### `main.cpp`

The entry point and the only place the stages are wired together: parse arguments, `build_tree`, then hand the tree to an `Executor`. Read this first.

### `core/`

Module `core`, is the data model everything else is phrased in terms of. `DirectoryTree` is the snapshot: a flat `files` container of `FileNode` and a `directories` container of `DirectoryNode`, plus collected `ErrorRecord`s. Nodes are addressed by `FileId` / `DirectoryId` (both `std::size_t` indices), never by pointer. `Action` (a variant of `SummaryAction`, `DuplicateAction`, …) names one requested report; `ErrorKind` is the error type carried across `std::expected` boundaries.

### `parser/`

Module `parser`. `parse` turns `argv` into a `ParseResult`- a list of `Action`s. The set of flags lives in one `flag_table`; each `handle_*` maps a flag to its `Action`.

### `builder/`

Turns a path into a `DirectoryTree`. `build_tree` is the entry point. It runs on a work-stealing thread pool: `ThreadPool` and `Worker` in the `builder:scheduler` partition (`scheduler.cpp`), one task per directory.

`deque.cpp` (module `phanes_deque`) physically lives here but is a standalone utility: `LockFreeDeque`, a Chase–Lev work-stealing deque constrained to `DequeElement` values, with `EpochDomain` / `EpochGuard` handling safe memory reclamation. It backs both the scheduler and the duplicate scanner.

### `analyzer/`

Module `analyzer`, the reports. The simple ones are one function each over the tree: `compute_file_stats`, `compute_directory_metrics`, `compute_directory_stats`, and the top-N / recent / extension queries. Look here first for "where is report X computed".

Duplicate detection is the outlier, in `duplicates.cpp`: `compute_duplicate_groups` (a `std::generator<DuplicateGroup>`) drives `group_files_by_size` → `prefilter_group` (sample hash) → `hash_file` (full hash). It is the only analyzer that reads file *contents*.

`hasher.cpp` (module `phanes_hasher`) is a standalone AVX2 content hash used only by the duplicate scanner: `PhanesHashState` with `phanes_hash_reset` / `_update` / `_digest`.

### `io/`

Module `phanes_io`. `Ring` is an async file-read interface — `submit` a read, drain `Result`s with `next`. Two implementations back it, selected by CMake at build time: `uring.cpp` (Linux, `io_uring`) and `oring.cpp` (Windows, `IoRing`). Used only by the duplicate scanner.

### `view/`

Module `view`: the `print_*` functions, each rendering one report to an `std::ostream`, plus `format_size` / `format_duration`. `executor.ixx` (module `executor`) lives here too: `Executor` is the visitor over `Action` that pairs each report's analyzer with its `print_*` and runs the requested set concurrently.

### `tests/`, `benchmark/`

GoogleTest unit tests and a Google Benchmark suite; files mirror the module they cover (`io_test.cpp`, `bench_io.cpp`, …).

## Invariants

- **The `DirectoryTree` is built once, then never mutated.** `build_tree` is the only writer; `analyzer`, `view`, and `executor` receive it `const`. This is what makes every report safe to run concurrently, and it is not obvious from any single file.
- **The builder never opens file contents** — only metadata (size, timestamps). The duplicate scanner is the *only* code that reads bytes from files.
- **`analyzer` does no output and `view` does no computation.** Analyzers return plain results; `print_*` functions only format. Neither touches the other's concern.
- **Nothing in `analyzer` names a platform I/O primitive.** It sees `Ring` only; there are no `io_uring` / `IoRing` symbols and no platform `#ifdef` in the scanner.
- **`core` depends on nothing.** Every module points at it; it points back at none, so the dependency graph is acyclic.

## Boundaries

- **`Ring` (module `phanes_io`)** is the seam between the duplicate scanner and the operating system's async I/O. Everything about `io_uring` vs `IoRing`  ring memory, submission, completion lives behind it; the build picks one implementation and the scanner never learns which.
- **`Action` (module `core`)** is the seam between `parser` and `executor`: the parser only produces `Action`s, the executor only consumes them. Adding a report means adding an `Action`, a flag, an analyzer function, a `print_*`, and an `Executor` case
  — nothing else moves.
- **The `tree → result → text` flow** keeps compute and presentation apart; `Executor` is the single place they meet.

## Cross-cutting concerns

- **Concurrency.** Three phases, each parallel in its own way: the build (thread pool, one task per directory), the reports (`std::async`, one per `Action`), and the duplicate scan (`LockFreeDeque` of size-groups, results streamed out through the `compute_duplicate_groups` coroutine). All of it rests on the read-only-tree invariant above.
- **Error handling.** Failures cross boundaries as `std::expected<_, ErrorKind>`; filesystem errors met during the scan are collected into the tree's `ErrorRecord`s rather than aborting.
- **Platform differences** are confined to two places: the `Ring` backend selected in CMake, and cloud-placeholder (OneDrive) file skipping on Windows in the scanner.
- **Modules.** The project is C++23 modules, roughly one per directory; a few utility modules (`phanes_deque`, `phanes_hasher`, `phanes_io`) are standalone and named independently of the directory they sit in.
