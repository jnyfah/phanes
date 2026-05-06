#include <atomic>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

import phanes_deque;

// ============================================================================
// Deque microbenchmarks
// ============================================================================

// Fresh deque every iteration: includes construction and any resize path.
static void BM_Deque_PushPop_Fresh(benchmark::State& state)
{
    const auto N = static_cast<std::size_t>(state.range(0));

    for (auto _ : state)
    {
        LockFreeDeque<std::size_t> deque;

        for (std::size_t i = 0; i < N; ++i)
            deque.push_back(i);
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < N; ++i)
            benchmark::DoNotOptimize(deque.pop_back());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(N) * 2);
}
BENCHMARK(BM_Deque_PushPop_Fresh)->Arg(64)->Arg(512)->Arg(4096)->Unit(benchmark::kNanosecond);

// Steady-state deque: pre-grow once, then time only owner push/pop.
static void BM_Deque_PushPop_Steady(benchmark::State& state)
{
    const auto N = static_cast<std::size_t>(state.range(0));
    LockFreeDeque<std::size_t> deque;

    for (std::size_t i = 0; i < N; ++i)
        deque.push_back(i);
    for (std::size_t i = 0; i < N; ++i)
        benchmark::DoNotOptimize(deque.pop_back());

    for (auto _ : state)
    {
        for (std::size_t i = 0; i < N; ++i)
            deque.push_back(i);
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < N; ++i)
            benchmark::DoNotOptimize(deque.pop_back());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(N) * 2);
}
BENCHMARK(BM_Deque_PushPop_Steady)->Arg(64)->Arg(512)->Arg(4096)->Unit(benchmark::kNanosecond);

// Contention stress: owner does push/pop while thieves spin on steal_front.
// Thread creation is excluded from timed measurement.
static void BM_Deque_StealContention(benchmark::State& state)
{
    const auto num_thieves = static_cast<int>(state.range(0));
    constexpr std::size_t N = 512;

    LockFreeDeque<std::size_t> deque;
    std::atomic start{false};
    std::atomic stop{false};

    std::vector<std::jthread> thieves;
    thieves.reserve(static_cast<std::size_t>(num_thieves));

    for (int t = 0; t < num_thieves; ++t)
    {
        thieves.emplace_back(
            [&]() noexcept
            {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();

                while (!stop.load(std::memory_order_relaxed))
                    benchmark::DoNotOptimize(deque.steal_front());
            });
    }
    start.store(true, std::memory_order_release);

    for (auto _ : state)
    {
        for (std::size_t i = 0; i < N; ++i)
            deque.push_back(i);
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < N; ++i)
            benchmark::DoNotOptimize(deque.pop_back());
        benchmark::ClobberMemory();
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thieves)
        t.join();

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(N) * 2);
}
BENCHMARK(BM_Deque_StealContention)->Arg(0)->Arg(1)->Arg(4)->Arg(8)->Unit(benchmark::kNanosecond);

// ============================================================================
// False-sharing benchmark
// ============================================================================

namespace
{

struct PackedCounters
{
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};

struct PaddedCounters
{
    alignas(64) std::atomic<std::uint64_t> a{0};
    alignas(64) std::atomic<std::uint64_t> b{0};
};

template <typename Counters>
void run_false_sharing_bench(benchmark::State& state)
{
    constexpr std::size_t iters = 1'000'000;

    for (auto _ : state)
    {
        Counters counters{};

        std::jthread t1(
            [&]()
            {
                for (std::size_t i = 0; i < iters; ++i)
                    counters.a.fetch_add(1, std::memory_order_relaxed);
            });
        std::jthread t2(
            [&]()
            {
                for (std::size_t i = 0; i < iters; ++i)
                    counters.b.fetch_add(1, std::memory_order_relaxed);
            });

        t1.join();
        t2.join();

        benchmark::DoNotOptimize(counters.a.load(std::memory_order_relaxed));
        benchmark::DoNotOptimize(counters.b.load(std::memory_order_relaxed));
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(iters) * 2);
}

} // namespace

static void BM_FalseSharing_Packed(benchmark::State& state)
{
    run_false_sharing_bench<PackedCounters>(state);
}
BENCHMARK(BM_FalseSharing_Packed)->Unit(benchmark::kMicrosecond);

static void BM_FalseSharing_Padded(benchmark::State& state)
{
    run_false_sharing_bench<PaddedCounters>(state);
}
BENCHMARK(BM_FalseSharing_Padded)->Unit(benchmark::kMicrosecond);
