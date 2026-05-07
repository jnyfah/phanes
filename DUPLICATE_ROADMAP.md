# Duplicate File Scanner — Implementation Roadmap

The builder taught you **how to walk a filesystem concurrently**.  
This feature teaches you **what it costs to read one**.

The checkpoints below follow the same pattern as builder: each one is a working,
testable slice that is useful on its own before the next layer is added.

---

## The Algorithm (before writing any code)

Naïve approach: hash every file, group by hash.  
Problem: hashing a 4 GB video file to discover it has no duplicate is pure waste.

The right approach is a **two-phase funnel**:

```
All files
  │
  ▼ Phase 1 — group by size (free, metadata only, no I/O)
Files with at least one size-match
  │
  ▼ Phase 2 — hash only those files
Files with at least one hash-match  ← these are your duplicates
```

This is the core insight. In a typical home directory, fewer than 5% of files
share a size with another file. You avoid reading 95% of file content entirely.
Every checkpoint builds toward this funnel.

---

## Checkpoint 1 — Size Grouping

**What you build:** A function that takes the existing `DirectoryTree` and returns
only the files worth hashing — grouped by size, each group having ≥ 2 members.

**Where it lives:** `src/analyzer/duplicates.cpp` (new file)

**Signature to aim for:**
```cpp
// Groups files by size. Returns only groups with 2+ members.
auto group_files_by_size(const DirectoryTree& tree)
    -> std::vector<std::vector<FileId>>;
```

**C++23 you will use here:**  
`std::ranges::chunk_by` — after sorting file IDs by size, this splits the sorted
range into consecutive groups of equal size. No manual index tracking. This is
the idiom that makes ranges worth learning.

```cpp
// rough shape — you will flesh this out
auto sorted = tree.files  // copy IDs, sort by file size
    | std::ranges::to<std::vector>();

std::ranges::sort(sorted, {}, [&](FileId id) {
    return tree.files[id].size;
});

auto groups = sorted
    | std::ranges::chunk_by([&](FileId a, FileId b) {
        return tree.files[a].size == tree.files[b].size;
    });
```

**Why this checkpoint exists:**  
No file is opened. No bytes are read. This step is pure metadata work — you are
just rearranging IDs you already have. Getting it right and tested in isolation
means that if hashing later produces wrong results, you know the grouping is not
the problem.

**CS:APP — read before this checkpoint:**  
Chapter 6.1–6.2 (Storage Technologies and Locality).  
Understand what "random access" vs "sequential access" costs before you start
reading files in the next checkpoint. The numbers in those sections are the
reason the two-phase funnel exists.

**Done when:** You can call this on the test fixture and print the groups. No
hashing yet.

---

## Checkpoint 2 — Hashing a Single File

**What you build:** A standalone function that opens one file, reads it in
chunks, and returns a hash. Nothing parallel. Nothing integrated. Just this.

**Where it lives:** `src/analyzer/duplicates.cpp`

**Signature to aim for:**
```cpp
struct HashError { std::string reason; };
using Hash = std::uint64_t;

auto hash_file(const std::filesystem::path& path) -> std::expected<Hash, HashError>;
```

**What hash function to use:**  
[xxHash](https://github.com/Cyan4973/xxHash) — `XXH3_64bits`. It is not
cryptographic (you do not need that — you are comparing files, not storing
passwords) and it runs at memory-bandwidth speed using SIMD internally. Add it
as a header-only include.

**The buffer size question:**  
You will need to pick a read buffer size. Start with 64 KB. Later, after
Checkpoint 4, you will profile different sizes and understand why it matters.
The answer is in CS:APP Chapter 6.

**C++23 you will use here:**  
`std::expected<Hash, HashError>` — the file might not be readable (permissions,
file vanishes between scan and hash). `std::expected` lets you return either the
hash or an error without throwing. This is the C++23 replacement for
`std::pair<bool, T>` or out-parameters.

**Why this checkpoint exists:**  
Hashing is the only genuinely new operation in this feature — everything else is
wiring. Getting it right and fast in isolation means you can measure it (how fast
does it hash a 100 MB file?) before introducing parallelism. Measurement before
optimization is the discipline.

**CS:APP — read before this checkpoint:**  
Chapter 10.1–10.4 (Files, Opening and Closing Files, Reading and Writing Files,
Robust I/O).  
You are about to call `read()` in a loop. That chapter explains what actually
happens when you do — kernel buffers, short counts, the difference between what
you asked for and what you got. The `rio_readn` pattern in that chapter is
directly applicable.

**Done when:** `hash_file("some/large/file")` returns a stable hash across
multiple calls. Write a small test that hashes the same file twice and asserts
the results match.

---

## Checkpoint 3 — Full Sequential Deduplication (End-to-End, Single Thread)

**What you build:** Wire Checkpoint 1 + Checkpoint 2 together into a working
`compute_duplicate_groups()` function, then hook up the CLI flag and output.

**Where it lives:**
- `src/analyzer/duplicates.cpp` — the computation
- `src/parser/parser.cpp` — add `--duplicates` flag → `DuplicatesAction`
- `src/view/view.cpp` — print the groups
- `src/view/executor.ixx` — add as a lazy async job (matching existing pattern)

**Signature to aim for:**
```cpp
// Returns groups of FileIds that are byte-for-byte identical.
auto compute_duplicate_groups(const DirectoryTree& tree)
    -> std::vector<std::vector<FileId>>;
```

**The flat_map moment:**  
Inside this function you will build a `std::flat_map<Hash, std::vector<FileId>>`.
This is C++23's cache-friendly map (sorted vector under the hood, not a red-black
tree). Use it here, then read why it is faster than `std::unordered_map` for this
workload — the answer connects directly to what you learned in Chapter 6.

**Why this checkpoint exists:**  
Single-threaded correct results first. This is the rule. Parallel code with a
bug is almost impossible to debug. Single-threaded code with a bug is just a bug.
Once this works and the output is correct for a known test directory, you have a
correctness baseline you can compare against in Checkpoint 4.

**CS:APP — read before this checkpoint:**  
Chapter 6.4 (The Memory Mountain) and 6.5 (Writing Cache-Friendly Code).  
You now have a working sequential hasher. Chapter 6.4 shows you how to measure
what your access pattern actually costs. Chapter 6.5 gives you the mental model
for why sequential reads of a file are fast — spatial locality, prefetcher
behaviour.

**Done when:** `phanes . --duplicates` produces correct output on a directory
where you have manually placed known duplicate files. Results match `fdupes` or
`jdupes` on the same directory.

---

## Checkpoint 4 — Parallel Hashing with Bounded Concurrency

**What you build:** Hash the size-groups in parallel using the existing
`ThreadPool`, but add a `std::counting_semaphore` to cap how many files are
being hashed concurrently.

**Why the semaphore:**  
You will be tempted to just throw all the work at the thread pool and let it run.
Do this first — measure it. Then add the semaphore and measure again. On an SSD,
unbounded parallel reads will likely be slower past 4–8 concurrent readers
because you saturate I/O queue depth and the kernel's I/O scheduler starts
serialising requests anyway. The semaphore gives you the knob to tune this.

This is the lesson: **I/O parallelism and CPU parallelism have different optimal
concurrency levels.** Your thread pool is sized for CPU work (one thread per
core). Your semaphore should be sized for I/O throughput (2–8 on an SSD,
1–2 on an HDD).

**C++20/23 you will use here:**  
`std::counting_semaphore<N>` — acquire before hashing, release when done.
It is `std::counting_semaphore<8>` for an SSD. Tune N by measuring.

```cpp
// rough shape
std::counting_semaphore<8> io_slots{8};

for (auto& group : size_groups) {
    pool.submit([&, group] {
        io_slots.acquire();
        auto result = hash_files_in_group(group, tree);
        io_slots.release();
        // merge result into output...
    });
}
```

**Profiling task:**  
Before and after adding the semaphore, time a run on a directory with thousands
of files. Record: wall time, CPU time (from `/usr/bin/time -v`), and whether CPU
utilisation goes up or down with more threads. The numbers will surprise you.

**CS:APP — read before this checkpoint:**  
Chapter 12.3–12.5 (Concurrent Programming with Threads, Shared Variables,
Semaphores).  
You have used semaphores in concept through the counting_semaphore above. These
sections explain the formal model behind them — why acquire/release works as a
mutual exclusion primitive and how to reason about them without races.

**Done when:** Wall time on a large directory is measurably better than
Checkpoint 3. Verified that output is identical to the single-threaded baseline.

---

## Checkpoint 5 — Partial Hashing Optimisation

**What you build:** For files in the same size group, hash only the first 4 KB
first. Only proceed to a full hash if two files produce the same partial hash.

**Why this matters:**  
Consider a directory full of 10,000 log files that are all exactly 1 MB but all
different. Phase 1 puts them all in one group. Phase 2 (full hash) reads all
10 GB. With partial hashing, you read 40 MB (4 KB × 10,000), find no collisions
in the partial hashes, and stop. You never read the remaining 9.96 GB.

**The shape of the change:**  
```
size_group → partial_hash_group → full_hash_group → duplicates
```
Add a middle phase. Hash first N bytes only. Re-group by partial hash. Then full
hash only the sub-groups that have ≥ 2 partial hash matches.

**What to tune:**  
The 4 KB threshold is a starting point. One read of 4 KB is one page (OS page
size is typically 4 KB — covered in CS:APP Chapter 9). Experiment: does 4 KB
beat 512 bytes? Does 16 KB beat 4 KB? Measure on your actual data.

**CS:APP — read before this checkpoint:**  
Chapter 9.1–9.3 (Physical and Virtual Addressing, Address Spaces, VM as a Tool
for Caching).  
The page size is not arbitrary — it is the unit the kernel moves data in. When
you read 4 KB from a file, you are asking for exactly one page. Understanding
why the OS deals in pages explains why 4 KB is a natural threshold for partial
hashing. Chapter 9 is also the foundation for understanding `mmap`, which is the
alternative to `read()` for hashing (a future experiment if you want one).

**Done when:** On a directory containing many large same-size-but-different files,
Checkpoint 5 is measurably faster than Checkpoint 4. Output still matches the
ground truth from Checkpoint 3.

---

## Where Each File Lives

```
src/
  analyzer/
    duplicates.cpp        ← all five checkpoints live here
  parser/
    parser.cpp            ← add --duplicates flag (Checkpoint 3)
  view/
    view.cpp              ← add print_duplicates() (Checkpoint 3)
    executor.ixx          ← wire as lazy async job (Checkpoint 3)
tests/
  test_duplicates.cpp     ← new test file, add from Checkpoint 1 onward
```

No changes to `core/`. No hash field on `FileNode`. The hash map is internal to
the analyzer and lives only for the duration of a `--duplicates` run.

---

## CS:APP Reading Summary

| Checkpoint | Read before starting |
|---|---|
| 1 — Size grouping | Ch 6.1–6.2 (Storage & Locality) |
| 2 — Single file hashing | Ch 10.1–10.4 (System-Level I/O) |
| 3 — Sequential end-to-end | Ch 6.4–6.5 (Memory Mountain, Cache-Friendly Code) |
| 4 — Parallel hashing | Ch 12.3–12.5 (Threads, Semaphores) |
| 5 — Partial hashing | Ch 9.1–9.3 (Virtual Memory as Cache) |

You are already working through the book front-to-back. These chapters will
arrive naturally in roughly this order anyway — the checkpoints are designed to
land just after the theory, so you implement the concept while it is fresh.

---

## Definition of Done (the whole feature)

- `phanes . --duplicates` works correctly and matches `jdupes` on the same path
- Checkpoint 3 (single-thread) output is used as ground truth for all later runs
- Checkpoint 4 shows measurable speedup over Checkpoint 3 on a large directory
- Checkpoint 5 shows measurable speedup over Checkpoint 4 on a directory with
  many large same-size-but-different files
- No new fields on `FileNode` or `DirectoryTree`
- New test file covers at minimum: empty directory, no duplicates, one duplicate
  pair, all files identical
