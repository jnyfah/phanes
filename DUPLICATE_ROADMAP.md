

What "optimized for this project" actually means here:

A general hash function like xxHash handles any input. Your use case has constraints you can exploit:

You're always reading from disk sequentially — your hash can be designed around streaming chunks, not random access
You hash many files back-to-back — you can reuse a persistent state object instead of reinitializing per file, avoiding setup cost
After Checkpoint 5 (partial hashing), you hash only the first 4 KB of most files — a function tuned for short inputs can outperform one tuned for large ones
What you'd actually be building:

Something like a streaming 64-bit hash with explicit SIMD — process 32 bytes per loop iteration using the same idea as xxHash but written by you, for this workload. You'd learn:

How bit mixing works (why you XOR, rotate, and multiply specific constants)
What SIMD intrinsics look like in C++ (__m256i, _mm256_xor_si256)
Why the magic constants in xxHash are those specific numbers (they're chosen to maximize avalanche effect — a 1-bit input change flips ~50% of output bits)




TODO
1. generator instead of vector  - coroutines 
2. Mmap?
3. read chapter 9
4. read chapter 12
5. i may be over threading ? so bench mark - i added more threads but check windows 
coroutines with iouring? but what of windows 






# Custom Hash Function Roadmap

## Goal
A 64-bit streaming hash tailored to phanes:
- Reusable state per thread (already how xxHash is used)
- Fast on sequential 256KB chunks (the hot path in hash_file)
- Fast on short inputs ≤ 12KB (the sample_hash_file path)
- Lives in src/analyzer/ as a module partition

Interface to match (so swapping in is one line in duplicates.cpp):
```cpp
struct PhanesHashState { ... };
void phanes_hash_reset(PhanesHashState* state);
void phanes_hash_update(PhanesHashState* state, const void* data, size_t len);
uint64_t phanes_hash_digest(const PhanesHashState* state);
```

---

## Before you start
- Finish Chapter 5 (optimizing program performance — critical path, ILP, loop unrolling)
- Finish Chapter 9 (virtual memory — page size, cache, why your buffer sizes are what they are)
- Chapter 5 is the critical one. Chapter 9 is background you already partly understand.

---

## Checkpoint 1 — One accumulator, no SIMD

**What you build:**
A scalar 64-bit streaming hash. One accumulator, process 8 bytes at a time.

```
for each 8-byte word in input:
    acc ^= word * PRIME_1
    acc  = rotate_left(acc, 31)
    acc *= PRIME_2

finalize:
    acc ^= acc >> 33
    acc *= PRIME_3
    acc ^= acc >> 29
```

**What you learn:**
- What bit mixing means: XOR, multiply, rotate scatter bits across the full 64-bit width
- Why multiply: it propagates a 1-bit change across many bit positions (unlike addition)
- Why rotate: prevents the mix from being undone by the same operation
- What "avalanche" means: flip one input bit, roughly half the output bits should change
- The finalization step: "avalanche" — smear remaining correlations

**What to do after:**
Write a test:
You'll find yours is slower. That's the point — now you know *why*.

---

## Checkpoint 2 — Understand the bottleneck (read this before coding checkpoint 3)

Run the benchmark. Look at the timing. Your hash is slow because of a **dependency chain**:

```
iteration 1: acc = f(acc, word1)  — must finish before iteration 2 starts
iteration 2: acc = f(acc, word2)  — depends on iteration 1
iteration 3: acc = f(acc, word3)  — depends on iteration 2
```

Every iteration depends on the previous one. The CPU cannot start iteration N+1 until iteration N is complete. This is the critical path Chapter 5 talks about.

xxHash's solution: multiple independent accumulators.

```
acc0 = f(acc0, word0)  ──┐
acc1 = f(acc1, word1)  ──┤── all four execute in parallel (no dependencies between them)
acc2 = f(acc2, word2)  ──┤
acc3 = f(acc3, word3)  ──┘
```

Four lanes, each independent. The CPU can issue all four operations simultaneously.

**No code in this checkpoint** — just benchmark checkpoint 1 and understand what the profiler/timing tells you.

---

## Checkpoint 3 — Four accumulators (ILP)

**What you build:**
Process 32 bytes per iteration across 4 independent accumulators.

```cpp
struct PhanesHashState {
    uint64_t acc[4];
    uint8_t  buffer[32];   // for leftover bytes < 32
    size_t   buf_used;
    size_t   total_len;
};
```

```
for each 32-byte block:
    acc[0] = mix(acc[0], load_u64(input + 0))
    acc[1] = mix(acc[1], load_u64(input + 8))
    acc[2] = mix(acc[2], load_u64(input + 16))
    acc[3] = mix(acc[3], load_u64(input + 24))

finalize: merge acc[0..3] into one 64-bit value
```

**What you learn:**
- Instruction-level parallelism (ILP) — the CPU executes independent instructions simultaneously
- How to handle tail bytes (the leftover < 32 bytes at end of input — short files, end of chunk)
- How to merge multiple accumulators into one final hash

**Expected result:**
Should be significantly faster than checkpoint 1, approaching xxHash territory.

---

## Checkpoint 4 — SIMD with AVX2

**What you build:**
Replace the 4 scalar accumulators with a single __m256i (256-bit register = 4 × 64-bit lanes).
Process 32 bytes in one SIMD instruction instead of 4 scalar instructions.

```cpp
#include <immintrin.h>   // AVX2 intrinsics

__m256i acc = _mm256_set_epi64x(SEED3, SEED2, SEED1, SEED0);

for each 32-byte block:
    __m256i data = _mm256_loadu_si256((__m256i*)input);
    data = _mm256_mullo_epi32(data, prime_vec);
    acc  = _mm256_xor_si256(acc, data);
    acc  = rotate_left_256(acc, 31);   // you'll write this helper
    acc  = _mm256_mullo_epi32(acc, prime2_vec);
```

**What you learn:**
- What `__m256i` is: a 256-bit value that the CPU treats as a vector of smaller integers
- SIMD intrinsics naming: `_mm256_` prefix, `_si256` suffix, operation in the middle
- `_mm256_loadu_si256`: load 32 bytes from unaligned memory into a register
- `_mm256_xor_si256`: XOR all 4 lanes simultaneously
- `_mm256_mullo_epi32`: multiply all 4 lanes simultaneously
- How to do a 64-bit lane rotation using SIMD (it's not a single instruction — you combine shift + OR)
- How to reduce a __m256i back to a single 64-bit value at finalization

**Guard with a feature check:**
```cpp
#ifdef __AVX2__
    // SIMD path
#else
    // fall back to checkpoint 3 scalar path
#endif
```

**Expected result:**
The compiler can now issue one SIMD instruction to do what previously took 4 instructions. Throughput doubles (or better) on large files.

---

## Checkpoint 5 — Benchmark and compare

Run bench_duplicates.cpp with your hash replacing xxHash. Measure:
- Throughput in GB/s on large files (hash_file path)
- Latency on 12KB inputs (sample_hash_file path)
- Compare wall time of the full --duplicates scan

You will not beat xxHash on large files — xxHash3 uses AVX-512 or AVX2 with hand-tuned constants and additional tricks (secret key mixing, striped layout). The goal is to understand *why* you can't beat it and what tricks it uses that you haven't.

On short inputs (≤ 12KB) you may get competitive results because xxHash3's short-input path is different from its streaming path.

---

## Magic constants — read this before checkpoint 1

The primes used in mixing are not arbitrary. They're chosen so that:
1. They're odd (no factor of 2, so multiplication doesn't lose bits)
2. They have good bit distribution (roughly equal 0s and 1s)
3. They maximize avalanche (empirically tested with SMHasher)

xxHash3's constants:
```
PRIME64_1 = 0x9E3779B185EBCA87  (golden ratio × 2^64, rounded to odd)
PRIME64_2 = 0xC2B2AE3D27D4EB4F
PRIME64_3 = 0x165667B19E3779F9
```

You can pick your own odd 64-bit constants, but the avalanche quality will be lower until you run them through a testing tool like SMHasher.

---

## Where the code lives

```
src/analyzer/
    analyzer.ixx     — exports PhanesHashState + the three functions
    hasher.cpp       — module analyzer:hasher; (the implementation)
    duplicates.cpp   — swaps XXH3_state_t for PhanesHashState at checkpoint 5
```

`hasher.cpp` starts with:
```cpp
module;
#include <immintrin.h>   // for SIMD
#include <cstdint>
#include <cstring>       // memcpy for unaligned loads
module analyzer:hasher;
```

`duplicates.cpp` imports it with:
```cpp
import :hasher;
```




notes on hash 

what is hash??? , take a file and produce a single 64bit number

how then are hash algorithms written 

uint64_t bad_hash(const uint8_t* data, size_t len) {
    uint64_t acc = 0;
    for (size_t i = 0; i < len; i++)
        acc += data[i];       // just add everything
    return acc;
}


if this is a simple hash i think with this algo2 files can hash to same number 

we dont need it to be irreversible 
