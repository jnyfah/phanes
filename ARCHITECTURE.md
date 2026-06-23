# Phanes Architecture

This document explains how Phanes is put together: the modules, how data flows
through them, and the design decisions behind the parts that aren't obvious from
reading the code. For *usage* (flags, examples) see the [README](README.md).

---

## High-level flow

Phanes runs as a single pass:

```
            ┌──────────┐      ┌──────────┐      ┌───────────┐      ┌────────┐
 argv  ───► │  parser  │ ───► │ builder  │ ───► │ executor  │ ───► │  view  │ ───► stdout
            └──────────┘      └──────────┘      └───────────┘      └────────┘
              parse flags       scan the          run each          render each
              into Actions      filesystem        analysis          report
                                in parallel        (concurrently)
                                → DirectoryTree
```

1. **parser** turns the command line into a list of `Action`s (one per flag).
2. **builder** walks the directory tree in parallel and produces an immutable
   `DirectoryTree` snapshot.
3. **executor** takes the `DirectoryTree` and, for each requested `Action`, calls
   the matching **analyzer** function, then hands the result to **view**.
4. **view** formats each result as text and writes it out.

The `DirectoryTree` is built once and then only read, so every analysis can run
concurrently against it without locking.

---

## Modules

Phanes is built with C++23 modules. Each box below is a module (or partition).

| Module | Files | Responsibility |
|---|---|---|
| `core` | `core/core.ixx` | The shared data model: `DirectoryTree`, `FileNode`, `DirectoryNode`, IDs, error records. Everything depends on this. |
| `phanes_deque` | `builder/deque.cpp` | `LockFreeDeque<T>` - the work-stealing deque used by the scheduler and the duplicate scanner. |
| `builder` (+ `:scheduler`) | `builder/builder.ixx`, `builder.cpp`, `scheduler.cpp` | Parallel filesystem scan into a `DirectoryTree`. The `:scheduler` partition holds the work-stealing thread pool. |
| `analyzer` | `analyzer/analyzer.ixx`, `analyzer.cpp`, `duplicates.cpp` | All the computations: file stats, directory metrics, largest-N, recent, extensions, empty dirs, and duplicate detection. |
| `phanes_hasher` | `analyzer/hasher.cpp` | The custom SIMD content hash (`reset`/`update`/`digest`) used by the duplicate scanner. |
| `parser` | `parser/parser.ixx`, `parser.cpp` | Command-line parsing into `Action` variants. |
| `view` | `view/view.ixx`, `view.cpp` | Pure formatting: turns analysis results into printable text. |
| `executor` | `view/executor.ixx` | Glue: dispatches each `Action` to the right analyzer + view pair, runs them concurrently. |

`main.cpp` wires these together: parse → build → execute.

---

## The data model (`core`)

The whole program is organized around one idea: **nodes are referenced by index,
not by pointer.**

```cpp
struct DirectoryTree
{
    std::vector<FileNode>      files;        // indexed by FileId
    std::deque<DirectoryNode>  directories;  // indexed by DirectoryId
    std::deque<ErrorRecord>    errors;
    DirectoryId                root;
    // scan timestamps...
};
```

A `FileId` / `DirectoryId` is just an index into these containers. A directory
holds `std::vector<FileId>` and `std::vector<DirectoryId>` for its children
instead of pointers.

**Why indices instead of pointers:**

- **Cache-friendly** -nodes live in contiguous storage, so scans stream through
  memory instead of chasing pointers around the heap.
- **Cheap to share across threads** - an index is a trivially-copyable `size_t`.
  Analyses pass IDs around freely; no ownership or lifetime questions.
- **Stable** - `std::deque` doesn't invalidate references to existing elements on
  `push_back`, which matters because the builder appends directories from multiple
  threads while other code holds references to earlier ones.

This is a lightweight form of data-oriented design: structure-of-arrays-ish layout
with integer handles.

---

## The builder - parallel scanning

`build_tree(root)` produces a `DirectoryTree`. Internally it's a `Scanner` driving
a work-stealing thread pool (`builder:scheduler`).

The unit of work is **one directory**. The flow:

1. Each worker pops a directory ID, enumerates it with `std::filesystem::directory_iterator`,
   and collects files/subdirs/errors into **thread-local vectors** (no locking
   during the scan itself).
2. After scanning a directory, the worker takes the locks *once* to append its
   local results to the shared tree and submit the newly-found subdirectories as
   new tasks.
3. Termination is tracked with an `std::atomic_int active_tasks`. When it hits
   zero, every directory has been scanned and the main thread wakes up.

Key choices:

- **Metadata only, no file contents.** The builder reads sizes and timestamps from
  the directory entry's cached stat data. It never opens file contents, so it's
  fast and (on Windows) it doesn't hydrate cloud-backed placeholder files.
- **Two lock granularities** - a `shared_mutex` for the directory container
  (shared for per-node writes, exclusive only for `push_back`) and a plain `mutex`
  for the files/errors lists. Threads do real work locally and only contend briefly
  at flush time.

### The scheduler (`builder:scheduler`)

A fixed pool of `std::jthread`s, each owning a `LockFreeDeque` of pending task IDs.
A worker runs its own deque LIFO (`pop_back`); when empty, it **steals** from other
workers' deques (`steal_front`). Idle workers sleep on a condition variable until
new work is submitted or the pool stops. This keeps all cores busy even when the
directory tree is lopsided (one huge folder, many tiny ones).

---

## The lock-free deque (`phanes_deque`)

`LockFreeDeque<T>` is a Chase–Lev-style work-stealing deque, the backbone of both
the scheduler and the duplicate scanner.

- **One owner, many thieves.** The owning thread pushes and pops one end
  (`push_back` / `pop_back`); other threads steal from the other end
  (`steal_front`). This asymmetry is what lets the common case (owner push/pop)
  run with minimal synchronization.
- **Trivially-copyable elements only.** Enforced by the `DequeElement` concept.
  The deque stores plain values (we only ever put `size_t` indices in it), which
  keeps the atomics simple and correct.
- **`alignas(64)` on the hot atomics** (`front`, `back`, `buffer`) to put them on
  separate cache lines and avoid false sharing between the owner and the thieves.
- **Epoch-based reclamation (EBR) for growth.** When the deque grows, the old
  buffer can't be freed immediately - an in-flight thief may still be reading it.
  EBR solves this safely (see below) instead of leaking the old buffers.

### Memory reclamation - EBR

The hard part of a lock-free deque is freeing the old buffer after a grow without
racing an in-flight `steal_front`. Phanes uses **epoch-based reclamation**:

- An `EpochDomain` holds one global epoch counter plus a per-thread "local epoch"
  slot (`kInactive` when the thread is outside a critical section). Each slot is
  `alignas(64)` to avoid false sharing.
- Before a thief reads the buffer, an `EpochGuard` (RAII) publishes the current
  global epoch into its slot ("pins" the epoch); its destructor resets the slot to
  `kInactive`. While pinned, no buffer from that epoch can be freed.
- On grow, the owner **retires** the old buffer into a per-epoch bucket
  (`retired[epoch % 3]`) rather than deleting it, then tries to advance the epoch.
- `try_advance_epoch` scans every local slot; if none is pinned at the current
  epoch, it bumps the global epoch and frees the bucket from two epochs ago. The
  three-bucket ring guarantees a full grace period has elapsed, so anything freed
  is provably unreachable by any thief.

---

## The analyzer

Each report is a free function that takes the `DirectoryTree` (read-only) and
returns a plain result. They fall into a few shapes:

- **Single-pass aggregates** - `compute_file_stats`, `compute_directory_metrics`
  (one walk, accumulate).
- **Rank / top-N** - `compute_largest_N_Files`, `compute_largest_N_Directories`
  (need to see everything, then sort/partial-sort).
- **Filters** - `compute_recent_files`, `compute_empty_directories`.

Because they only read the tree, the executor can run several at once.

### Duplicate detection (`duplicates.cpp`)

The expensive one. It's a **funnel** that does the cheapest test first and only
pays for expensive work on survivors:

```
all files
   │  group by size            (free - sizes already known, no I/O)
   ▼
size groups (≥2 files)
   │  sample hash               (read ~12KB: front + middle + end)
   ▼
sample-hash groups (≥2 files)
   │  full-content hash         (read the whole file)
   ▼
confirmed duplicate groups
```

- Files unique in size can't be duplicates → discarded for free.
- The sample hash (a few KB from three places in the file) cheaply eliminates
  files that merely share a size but differ in content.
- Only the survivors get fully read and hashed.

Parallelism and output:

- Each **size group** is a task in a `LockFreeDeque`; worker `jthread`s steal
  groups and process them independently. Groups are sorted largest-first so the
  big jobs get distributed early (LPT scheduling), with a secondary sort by path
  for I/O locality.
- `compute_duplicate_groups` is a **`std::generator<DuplicateGroup>`**: workers
  push confirmed groups onto a shared queue, and the coroutine yields each one to
  the caller as it's found. This means results stream to the terminal as they're
  confirmed instead of appearing all at once at the end.
- On Windows, OneDrive/cloud placeholder files are detected via file attributes
  and skipped, so the scan doesn't trigger a mass download.

---

## The custom hash (`phanes_hasher`)

A non-cryptographic 64-bit content hash written specifically for the duplicate
scanner. It's a streaming API:

```cpp
PhanesHashState s;
phanes_hash_reset(s);
phanes_hash_update(s, data, len);   // call as many times as you like
uint64_t h = phanes_hash_digest(s);
```

Design notes:

- **Four independent accumulators** processed with AVX2 SIMD, so the CPU can run
  four mix-chains in parallel (instruction-level parallelism) instead of stalling
  on a single dependency chain.
- **Streaming** - `update` can be called with arbitrary chunk sizes; an internal
  32-byte buffer plus a block counter keep the result identical regardless of how
  the input is split across calls.
- It is *not* meant to beat xxHash; it exists to be fast *enough* and to be fully
  understood. (There's a whole blog post on how it was built and why it lands where
  it does.)

---

## The view + executor

**view** is pure formatting - each `print_*` function takes a result and an
`std::ostream` and writes text. No computation, no I/O beyond the stream.

**executor** is the orchestration layer:

- It holds the `DirectoryTree` and lazily caches shared derived data
  (`DirectoryMetrics`, `FileStats`, empty-dir list) so multiple reports don't
  recompute it.
- It's a visitor over the `Action` variant: `operator()(SummaryAction)`,
  `operator()(DuplicateAction)`, etc., each calling the right analyzer + view pair.
- `run()` launches the requested actions concurrently with `std::async` and prints
  their outputs in the order the flags were given.

---

## Threading model summary

| Stage | Concurrency |
|---|---|
| Parsing | single-threaded |
| Building the tree | work-stealing thread pool, one task per directory |
| Running reports | `std::async`, one task per requested flag |
| Duplicate scan | work-stealing deque, one task per size group; results streamed via coroutine |
| Hashing | per-thread hash state, SIMD within each file |

The invariant that makes all of this safe: **the `DirectoryTree` is written only
during the build phase, and read-only afterward.** Once `build_tree` returns,
nothing mutates it, so every downstream analysis is free to run in parallel without
synchronization.

---

## Where to read more

- Lock-free deque design - blog series (DFS → lock-free → benchmarks)
- The custom hash - "Dependency Chains, ILP and SIMD" post
- Build / usage / flags - [README](README.md)
