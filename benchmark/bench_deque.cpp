#include <atomic>
#include <benchmark/benchmark.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

// ============================================================================
// Self-contained LockFreeDeque — isolated copy for microbenchmarking
// ============================================================================

namespace
{

template <typename T>
concept BenchDequeElement = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

template <BenchDequeElement T>
class BenchLockFreeDeque
{
    struct Buffer
    {
        std::int64_t capacity;
        std::int64_t mask;
        std::unique_ptr<T[]> data;

        explicit Buffer(std::int64_t c)
            : capacity(c), mask(c - 1), data(std::make_unique<T[]>(static_cast<std::size_t>(c)))
        {
            assert(capacity > 0 && !(capacity & (capacity - 1)));
        }

        [[nodiscard]] auto resize(std::int64_t f, std::int64_t b) const -> std::unique_ptr<Buffer>
        {
            auto next = std::make_unique<Buffer>(capacity * 2);
            for (std::int64_t i = f; i < b; ++i)
                next->data[i & next->mask] = data[i & mask];
            return next;
        }
    };

  public:
    explicit BenchLockFreeDeque(std::int64_t cap = 1024)
    {
        auto first = std::make_unique<Buffer>(cap);
        buffer.store(first.get(), std::memory_order_relaxed);
        old_buffers.push_back(std::move(first));
    }

    BenchLockFreeDeque(const BenchLockFreeDeque&) = delete;
    auto operator=(const BenchLockFreeDeque&) -> BenchLockFreeDeque& = delete;

    void push_back(T item)
    {
        const auto b = back.load(std::memory_order_relaxed);
        const auto f = front.load(std::memory_order_acquire);
        auto* buf = buffer.load(std::memory_order_relaxed);

        if (b - f >= buf->capacity)
        {
            auto next = buf->resize(f, b);
            auto* next_raw = next.get();
            old_buffers.push_back(std::move(next));
            buffer.store(next_raw, std::memory_order_release);
            buf = next_raw;
        }

        buf->data[b & buf->mask] = item;
        back.store(b + 1, std::memory_order_release);
    }

    auto pop_back() noexcept -> std::optional<T>
    {
        auto b = back.load(std::memory_order_relaxed) - 1;
        back.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        auto f = front.load(std::memory_order_relaxed);
        if (f > b)
        {
            back.store(f, std::memory_order_relaxed);
            return std::nullopt;
        }

        auto* buf = buffer.load(std::memory_order_relaxed);

        if (f == b)
        {
            if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                back.store(f, std::memory_order_relaxed);
                return std::nullopt;
            }
            back.store(f + 1, std::memory_order_relaxed);
        }

        return buf->data[b & buf->mask];
    }

    auto steal_front() noexcept -> std::optional<T>
    {
        auto f = front.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto b = back.load(std::memory_order_acquire);

        if (f >= b)
            return std::nullopt;

        auto* buf = buffer.load(std::memory_order_relaxed);
        const T value = buf->data[f & buf->mask];

        if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return std::nullopt;

        return value;
    }

    [[nodiscard]] auto empty() const noexcept -> bool
    {
        return front.load(std::memory_order_relaxed) >= back.load(std::memory_order_relaxed);
    }

  private:
    alignas(64) std::atomic<std::int64_t> front{0};
    alignas(64) std::atomic<std::int64_t> back{0};
    alignas(64) std::atomic<Buffer*> buffer{nullptr};

    // Owner-only storage for old buffers to keep them alive after resize.
    std::vector<std::unique_ptr<Buffer>> old_buffers;
};

} // namespace

// ============================================================================
// Deque microbenchmarks
// ============================================================================

// Fresh deque every iteration: includes construction and any resize path.
static void BM_Deque_PushPop_Fresh(benchmark::State& state)
{
    const auto N = static_cast<std::size_t>(state.range(0));

    for (auto _ : state)
    {
        BenchLockFreeDeque<std::size_t> deque;

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
    BenchLockFreeDeque<std::size_t> deque;

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

    BenchLockFreeDeque<std::size_t> deque;
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
