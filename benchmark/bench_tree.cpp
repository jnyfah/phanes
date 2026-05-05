#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
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

fs::path make_unique_bench_path(std::string_view stem)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
        std::format("{}_{}_{}",  stem, now, std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

} // namespace

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
        dir.path = std::format("/bench/dir{}", d);
        dir.readable = true;

        for (std::size_t f = 0; f < files_per_dir; ++f)
        {
            const auto ext = kExtensions[(d * files_per_dir + f) % kNumExtensions];

            FileNode file{};
            file.id = fid;
            file.parent = did;
            file.path = dir.path / std::format("file{}{}", f, ext);
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
        empty.path = std::format("/bench/empty{}", e);
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
        auto dir = root / std::format("d{}", d);
        fs::create_directories(dir);
        for (int f = 0; f < files; ++f)
            std::ofstream{dir / std::format("f{}.txt", f)} << 'x';
    }
}

static void create_nested_tree(const fs::path& root, int l1, int l2_per_l1, int files)
{
    fs::remove_all(root);
    for (int i = 0; i < l1; ++i)
    {
        for (int j = 0; j < l2_per_l1; ++j)
        {
            auto dir = root / std::format("l1_{}", i) / std::format("l2_{}", j);
            fs::create_directories(dir);
            for (int f = 0; f < files; ++f)
                std::ofstream{dir / std::format("f{}.txt", f)} << 'x';
        }
    }
}

// ============================================================================
// Builder benchmarks
// ============================================================================

static void BM_BuildTree_ThreadOverhead(benchmark::State& state)
{
    const auto dirs = static_cast<int>(state.range(0));
    const auto root = make_unique_bench_path("phanes_bench_overhead");

    create_flat_tree(root, dirs, 1);

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * dirs);

    fs::remove_all(root);
}
BENCHMARK(BM_BuildTree_ThreadOverhead)->Arg(1)->Arg(5)->Arg(20)->Arg(100)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Granularity(benchmark::State& state)
{
    const auto dirs = static_cast<int>(state.range(0));
    const auto files = static_cast<int>(state.range(1));
    const auto root = make_unique_bench_path("phanes_bench_granularity");

    create_flat_tree(root, dirs, files);

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(dirs) * files);
    state.SetLabel(std::format("dirs={} files={}", dirs, files));

    fs::remove_all(root);
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

    create_flat_tree(root, 100, 100);

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 10000);

    fs::remove_all(root);
}
BENCHMARK(BM_BuildTree_Flat)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Nested(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_nested");

    create_nested_tree(root, 10, 10, 100);

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 10000);

    fs::remove_all(root);
}
BENCHMARK(BM_BuildTree_Nested)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Balanced(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_balanced");

    create_flat_tree(root, 100, 100);

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 10000);

    fs::remove_all(root);
}
BENCHMARK(BM_BuildTree_Balanced)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_Skewed(benchmark::State& state)
{
    const auto root = make_unique_bench_path("phanes_bench_skewed");

    auto heavy = root / "heavy";
    fs::create_directories(heavy);
    for (int f = 0; f < 800; ++f)
        std::ofstream{heavy / std::format("f{}.txt", f)} << 'x';

    for (int d = 0; d < 100; ++d)
    {
        auto dir = root / std::format("light_{}", d);
        fs::create_directories(dir);
        for (int f = 0; f < 2; ++f)
            std::ofstream{dir / std::format("f{}.txt", f)} << 'x';
    }

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));

    state.SetItemsProcessed(state.iterations() * 1000); // 800 heavy + 200 light

    fs::remove_all(root);
}
BENCHMARK(BM_BuildTree_Skewed)->Unit(benchmark::kMillisecond);

static void BM_BuildTree_ThreadScaling(benchmark::State& state)
{
    const auto num_threads = static_cast<std::size_t>(state.range(0));
    const auto root = make_unique_bench_path("phanes_bench_thread_scaling");

    create_flat_tree(root, 200, 50);

    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root, num_threads));

    state.SetItemsProcessed(state.iterations() * 10000);
    state.SetLabel(std::to_string(num_threads) + " thread(s)");

    fs::remove_all(root);
}
BENCHMARK(BM_BuildTree_ThreadScaling)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
