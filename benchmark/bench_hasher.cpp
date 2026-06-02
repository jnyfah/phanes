#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

import analyzer;
import phanes_hasher;

namespace
{

std::vector<uint8_t> make_data(std::size_t size)
{
    std::vector<uint8_t> buf(size);
    uint64_t s = 0xDEADBEEFCAFEBABEULL;
    for (auto& byte : buf)
    {
        s ^= s >> 33;
        s *= 0xFF51AFD7ED558CCDULL;
        s ^= s >> 33;
        byte = static_cast<uint8_t>(s);
    }
    return buf;
}

const std::vector<uint8_t> DATA_1MB = make_data(1024 * 1024);
const std::vector<uint8_t> DATA_12KB = make_data(12 * 1024);

} // namespace

// ============================================================
// 1MB — the hash_file hot path (large sequential reads)
// ============================================================

static void BM_PhanesHash_1MB(benchmark::State& bstate)
{
    PhanesHashState hs;
    for (auto _ : bstate)
    {
        benchmark::DoNotOptimize(DATA_1MB.data()); // input "could change" — block hoisting
        phanes_hash_reset(hs);
        phanes_hash_update(hs, DATA_1MB.data(), DATA_1MB.size());
        benchmark::DoNotOptimize(phanes_hash_digest(hs));
    }
    bstate.SetBytesProcessed(bstate.iterations() * static_cast<int64_t>(DATA_1MB.size()));
}
BENCHMARK(BM_PhanesHash_1MB)->Unit(benchmark::kMicrosecond);

// ============================================================
// 12KB — the sample_hash_file hot path
// ============================================================

static void BM_PhanesHash_12KB(benchmark::State& bstate)
{
    PhanesHashState hs;
    for (auto _ : bstate)
    {
        benchmark::DoNotOptimize(DATA_12KB.data());
        phanes_hash_reset(hs);
        phanes_hash_update(hs, DATA_12KB.data(), DATA_12KB.size());
        benchmark::DoNotOptimize(phanes_hash_digest(hs));
    }
    bstate.SetBytesProcessed(bstate.iterations() * static_cast<int64_t>(DATA_12KB.size()));
}
BENCHMARK(BM_PhanesHash_12KB)->Unit(benchmark::kMicrosecond);

