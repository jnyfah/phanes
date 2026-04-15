#include <atomic>
#include <benchmark/benchmark.h>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import core;
import builder;
import analyzer;
import view;

namespace fs = std::filesystem;

// ============================================================================
// Utility helpers
// ============================================================================

namespace
{

static fs::path make_unique_bench_path(std::string_view stem)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
        (std::string(stem) + "_" + std::to_string(now) + "_" +
         std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())));
}

template <typename T>
concept BenchDequeElement = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

} // namespace

// ============================================================================
// Self-contained LockFreeDeque — isolated copy for microbenchmarking
// ============================================================================

namespace
{

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
        {
            return std::nullopt;
        }

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
    const int num_thieves = static_cast<int>(state.range(0));
    constexpr std::size_t N = 512;

    BenchLockFreeDeque<std::size_t> deque;
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};

    state.PauseTiming();
    std::vector<std::thread> thieves;
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
    state.ResumeTiming();

    for (auto _ : state)
    {
        for (std::size_t i = 0; i < N; ++i)
            deque.push_back(i);
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < N; ++i)
            benchmark::DoNotOptimize(deque.pop_back());
        benchmark::ClobberMemory();
    }

    state.PauseTiming();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thieves)
        t.join();
    state.ResumeTiming();

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
static void run_false_sharing_bench(benchmark::State& state)
{
    constexpr std::size_t iters = 1'000'000;

    for (auto _ : state)
    {
        Counters counters{};

        std::thread t1(
            [&]()
            {
                for (std::size_t i = 0; i < iters; ++i)
                    counters.a.fetch_add(1, std::memory_order_relaxed);
            });
        std::thread t2(
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

// ============================================================================
// Synthetic DirectoryTree helpers
// ============================================================================

static const char* const kExtensions[] = {
    ".txt",
    ".cpp",
    ".hpp",
    ".py",
    ".json",
    ".md",
    ".rs",
    ".go",
    ".ts",
    ".js",
};
static constexpr std::size_t kNumExtensions = 10;

static DirectoryTree
make_synthetic_tree(std::size_t num_dirs, std::size_t files_per_dir, std::size_t num_empty_dirs = 0)
{
    DirectoryTree tree;

    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto old_ts = now - std::chrono::seconds{60LL * 60 * 24 * 30};

    DirectoryNode root{};
    root.id = 0;
    root.parent = std::nullopt;
    root.path = "/bench";
    root.readable = true;
    tree.directories.push_back(root);
    tree.root = DirectoryId{0};

    FileId fid = 0;
    DirectoryId did = 1;

    for (std::size_t d = 0; d < num_dirs; ++d)
    {
        DirectoryNode dir{};
        dir.id = did;
        dir.parent = DirectoryId{0};
        dir.path = "/bench/dir" + std::to_string(d);
        dir.readable = true;

        for (std::size_t f = 0; f < files_per_dir; ++f)
        {
            const auto ext = kExtensions[(d * files_per_dir + f) % kNumExtensions];

            FileNode file{};
            file.id = fid;
            file.parent = did;
            file.path = dir.path / ("file" + std::to_string(f) + std::string(ext));
            file.size = static_cast<std::uintmax_t>((d + 1) * (f + 1) * 1024);
            file.modified = (f % 5 == 0) ? old_ts : now;
            file.readable = true;
            file.is_symlink = (f % 10 == 0);

            dir.files.push_back(fid);
            tree.files.push_back(file);
            ++fid;
        }

        tree.directories[0].subdirs.push_back(did);
        tree.directories.push_back(dir);
        ++did;
    }

    for (std::size_t e = 0; e < num_empty_dirs; ++e)
    {
        DirectoryNode empty{};
        empty.id = did;
        empty.parent = DirectoryId{0};
        empty.path = "/bench/empty" + std::to_string(e);
        empty.readable = true;
        tree.directories[0].subdirs.push_back(did);
        tree.directories.push_back(empty);
        ++did;
    }

    tree.scan_started = now;
    tree.scan_finished = now;
    return tree;
}

// ============================================================================
// Analyzer benchmarks
// ============================================================================

static void BM_FileStats(benchmark::State& state)
{
    auto tree = make_synthetic_tree(static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_file_stats(tree));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(tree.files.size()));
}
BENCHMARK(BM_FileStats)->Args({50, 20})->Args({200, 50})->Args({500, 100})->Unit(benchmark::kMicrosecond);

static void BM_DirectoryMetrics(benchmark::State& state)
{
    auto tree = make_synthetic_tree(static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_directory_metrics(tree));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(tree.directories.size()));
}
BENCHMARK(BM_DirectoryMetrics)->Args({50, 20})->Args({200, 50})->Args({500, 100})->Unit(benchmark::kMicrosecond);

static void BM_EmptyDirs(benchmark::State& state)
{
    const auto num_dirs = static_cast<std::size_t>(state.range(0));
    const auto num_empty = num_dirs / 4;
    auto tree = make_synthetic_tree(num_dirs, 20, num_empty);

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_empty_directories(tree));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(tree.directories.size()));
}
BENCHMARK(BM_EmptyDirs)->Arg(50)->Arg(200)->Arg(500)->Unit(benchmark::kMicrosecond);

static void BM_LargestNFiles(benchmark::State& state)
{
    auto tree = make_synthetic_tree(200, 50);
    const auto n = static_cast<std::size_t>(state.range(0));

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_largest_N_Files(tree, n));
}
BENCHMARK(BM_LargestNFiles)->Arg(10)->Arg(50)->Arg(200)->Arg(500)->Unit(benchmark::kMicrosecond);

static void BM_LargestNDirs(benchmark::State& state)
{
    auto tree = make_synthetic_tree(500, 20);
    auto metrics = compute_directory_metrics(tree);
    const auto n = static_cast<std::size_t>(state.range(0));

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_largest_N_Directories(tree, metrics, n));
}
BENCHMARK(BM_LargestNDirs)->Arg(10)->Arg(50)->Arg(200)->Arg(500)->Unit(benchmark::kMicrosecond);

static void BM_ExtensionStats(benchmark::State& state)
{
    auto tree = make_synthetic_tree(static_cast<std::size_t>(state.range(0)), 50);

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_extension_stats(tree));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(tree.files.size()));
}
BENCHMARK(BM_ExtensionStats)->Arg(50)->Arg(200)->Arg(1000)->Unit(benchmark::kMicrosecond);

static void BM_RecentFiles(benchmark::State& state)
{
    auto tree = make_synthetic_tree(static_cast<std::size_t>(state.range(0)), 50);
    const auto window = std::chrono::seconds{60LL * 60 * 24 * 7};

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_recent_files(tree, window));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(tree.files.size()));
}
BENCHMARK(BM_RecentFiles)->Arg(50)->Arg(200)->Arg(1000)->Unit(benchmark::kMicrosecond);

static void BM_DirectoryStats(benchmark::State& state)
{
    auto tree = make_synthetic_tree(static_cast<std::size_t>(state.range(0)), 50);
    auto metrics = compute_directory_metrics(tree);

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_directory_stats(tree, metrics));
}
BENCHMARK(BM_DirectoryStats)->Arg(50)->Arg(200)->Arg(500)->Unit(benchmark::kMicrosecond);

static void BM_Summary(benchmark::State& state)
{
    const auto num_dirs = static_cast<std::size_t>(state.range(0));
    const auto num_empty = num_dirs / 4;
    auto tree = make_synthetic_tree(num_dirs, 50, num_empty);
    auto metrics = compute_directory_metrics(tree);
    auto empty = compute_empty_directories(tree);
    auto fstats = compute_file_stats(tree);

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_summary(tree, metrics, empty.size(), fstats));
}
BENCHMARK(BM_Summary)->Arg(50)->Arg(200)->Arg(500)->Unit(benchmark::kMicrosecond);

// ============================================================================
// Filesystem fixture helpers
// ============================================================================

static void create_flat_tree(const fs::path& root, int dirs, int files)
{
    fs::remove_all(root);
    for (int d = 0; d < dirs; ++d)
    {
        auto dir = root / ("d" + std::to_string(d));
        fs::create_directories(dir);
        for (int f = 0; f < files; ++f)
            std::ofstream{dir / ("f" + std::to_string(f) + ".txt")} << 'x';
    }
}

static void create_nested_tree(const fs::path& root, int l1, int l2_per_l1, int files)
{
    fs::remove_all(root);
    for (int i = 0; i < l1; ++i)
    {
        for (int j = 0; j < l2_per_l1; ++j)
        {
            auto dir = root / ("l1_" + std::to_string(i)) / ("l2_" + std::to_string(j));
            fs::create_directories(dir);
            for (int f = 0; f < files; ++f)
                std::ofstream{dir / ("f" + std::to_string(f) + ".txt")} << 'x';
        }
    }
}

// ============================================================================
// Builder benchmarks
// ============================================================================

static void BM_BuildTree_ThreadOverhead(benchmark::State& state)
{
    const int dirs = static_cast<int>(state.range(0));
    const auto root = make_unique_bench_path("phanes_bench_overhead");

    state.PauseTiming();
    create_flat_tree(root, dirs, 1);
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * dirs);

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_ThreadOverhead)->Arg(1)->Arg(5)->Arg(20)->Arg(100)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Granularity(benchmark::State& state)
{
    const int dirs = static_cast<int>(state.range(0));
    const int files = static_cast<int>(state.range(1));
    const auto root = make_unique_bench_path("phanes_bench_granularity");

    state.PauseTiming();
    create_flat_tree(root, dirs, files);
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(dirs) * files);
    state.SetLabel("dirs=" + std::to_string(dirs) + " files=" + std::to_string(files));

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_Granularity)
    ->Args({10, 200})
    ->Args({100, 20})
    ->Args({500, 4})
    ->Args({1000, 2})
    ->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Flat(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_flat");

    state.PauseTiming();
    create_flat_tree(root, 100, 100);
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 10000);

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_Flat)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Nested(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_nested");

    state.PauseTiming();
    create_nested_tree(root, 10, 10, 100);
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 10000);

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_Nested)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Balanced(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_balanced");

    state.PauseTiming();
    create_flat_tree(root, 100, 100);
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 10000);

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_Balanced)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Skewed(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_skewed");

    state.PauseTiming();
    fs::remove_all(root);

    auto heavy = root / "heavy";
    fs::create_directories(heavy);
    for (int f = 0; f < 800; ++f)
        std::ofstream{heavy / ("f" + std::to_string(f) + ".txt")} << 'x';

    for (int d = 0; d < 100; ++d)
    {
        auto dir = root / ("light_" + std::to_string(d));
        fs::create_directories(dir);
        for (int f = 0; f < 2; ++f)
            std::ofstream{dir / ("f" + std::to_string(f) + ".txt")} << 'x';
    }
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 1000); // 800 heavy + 200 light

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_Skewed)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_ThreadScaling(benchmark::State& state)
{
    const auto num_threads = static_cast<std::size_t>(state.range(0));
    const auto root = make_unique_bench_path("phanes_bench_thread_scaling");

    state.PauseTiming();
    create_flat_tree(root, 200, 50);
    state.ResumeTiming();

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root, num_threads));

    state.SetItemsProcessed(state.iterations() * 10000);
    state.SetLabel(std::to_string(num_threads) + " thread(s)");

    state.PauseTiming();
    fs::remove_all(root);
    state.ResumeTiming();
}
BENCHMARK(BM_BuildTree_ThreadScaling)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();