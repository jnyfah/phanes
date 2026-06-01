#include "xxhash.h"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

import analyzer;

namespace
{

// pseudo-random data generated once — fixed seed so results are reproducible
std::vector<uint8_t> make_data(std::size_t size)
{
    std::vector<uint8_t> buf(size);
    uint64_t state = 0xDEADBEEFCAFEBABEULL;
    for (auto& byte : buf)
    {
        state ^= state >> 33;
        state *= 0xFF51AFD7ED558CCDULL;
        state ^= state >> 33;
        byte = static_cast<uint8_t>(state);
    }
    return buf;
}

const std::vector<uint8_t> DATA_1MB  = make_data(1024 * 1024);
const std::vector<uint8_t> DATA_12KB = make_data(12 * 1024);

} // namespace

// ============================================================
// 1MB — the hash_file hot path (large sequential reads)
// ============================================================

static void BM_PhanesHash_1MB(benchmark::State& state)
{
    for (auto _ : state)
        benchmark::DoNotOptimize(phanes_hash(DATA_1MB.data(), DATA_1MB.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(DATA_1MB.size()));
}
BENCHMARK(BM_PhanesHash_1MB)->Unit(benchmark::kMicrosecond);

static void BM_XXHash_1MB(benchmark::State& state)
{
    for (auto _ : state)
        benchmark::DoNotOptimize(XXH3_64bits(DATA_1MB.data(), DATA_1MB.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(DATA_1MB.size()));
}
BENCHMARK(BM_XXHash_1MB)->Unit(benchmark::kMicrosecond);

// ============================================================
// 12KB — the sample_hash_file hot path (front+middle+end samples)
// At this size, per-call overhead and setup cost matter more than throughput.
// ============================================================

static void BM_PhanesHash_12KB(benchmark::State& state)
{
    for (auto _ : state)
        benchmark::DoNotOptimize(phanes_hash(DATA_12KB.data(), DATA_12KB.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(DATA_12KB.size()));
}
BENCHMARK(BM_PhanesHash_12KB)->Unit(benchmark::kMicrosecond);

static void BM_XXHash_12KB(benchmark::State& state)
{
    for (auto _ : state)
        benchmark::DoNotOptimize(XXH3_64bits(DATA_12KB.data(), DATA_12KB.size()));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(DATA_12KB.size()));
}
BENCHMARK(BM_XXHash_12KB)->Unit(benchmark::kMicrosecond);
