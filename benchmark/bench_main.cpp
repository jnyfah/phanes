#include <benchmark/benchmark.h>
#include <fstream>
#include <chrono>
#include <filesystem>

import core;
import builder;
import analyzer;
import view;

// ---------------------------------------------------------------------------
// Helpers — build synthetic DirectoryTree of configurable scale
// ---------------------------------------------------------------------------

static DirectoryTree make_synthetic_tree(std::size_t num_dirs, std::size_t files_per_dir) {
    DirectoryTree tree;

    auto now = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now());

    // root
    DirectoryNode root{};
    root.id       = 0;
    root.parent   = std::nullopt;
    root.path     = "/bench";
    root.readable = true;
    tree.directories.push_back(root);
    tree.root = DirectoryId{0};

    FileId   fid = 0;
    DirectoryId did = 1;

    for (std::size_t d = 0; d < num_dirs; ++d) {
        DirectoryNode dir{};
        dir.id       = did;
        dir.parent   = DirectoryId{0};
        dir.path     = "/bench/dir" + std::to_string(d);
        dir.readable = true;

        for (std::size_t f = 0; f < files_per_dir; ++f) {
            FileNode file{};
            file.id         = fid;
            file.parent     = did;
            file.path       = dir.path / ("file" + std::to_string(f) + ".txt");
            file.size       = static_cast<std::uintmax_t>((d + 1) * (f + 1) * 1024);
            file.modified   = now;
            file.readable   = true;
            file.is_symlink = (f % 10 == 0);

            dir.files.push_back(fid);
            tree.files.push_back(file);
            ++fid;
        }

        tree.directories[0].subdirs.push_back(did);
        tree.directories.push_back(dir);
        ++did;
    }

    tree.scan_started  = now;
    tree.scan_finished = now;
    return tree;
}

// ---------------------------------------------------------------------------
// Analyzer — compute_directory_metrics
// ---------------------------------------------------------------------------

static void BM_ComputeDirectoryMetrics_Small(benchmark::State& state) {
    auto tree = make_synthetic_tree(50, 20);
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_directory_metrics(tree));
}
BENCHMARK(BM_ComputeDirectoryMetrics_Small);

static void BM_ComputeDirectoryMetrics_Large(benchmark::State& state) {
    auto tree = make_synthetic_tree(500, 100);
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_directory_metrics(tree));
}
BENCHMARK(BM_ComputeDirectoryMetrics_Large);

// ---------------------------------------------------------------------------
// Analyzer — compute_largest_N_Files (partial sort)
// ---------------------------------------------------------------------------

static void BM_LargestNFiles(benchmark::State& state) {
    auto tree = make_synthetic_tree(200, 50);
    std::size_t n = static_cast<std::size_t>(state.range(0));
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_largest_N_Files(tree, n));
}
BENCHMARK(BM_LargestNFiles)->Arg(10)->Arg(50)->Arg(200);

// ---------------------------------------------------------------------------
// Analyzer — compute_largest_N_Directories
// ---------------------------------------------------------------------------

static void BM_LargestNDirectories(benchmark::State& state) {
    auto tree    = make_synthetic_tree(200, 50);
    auto metrics = compute_directory_metrics(tree);
    std::size_t n = static_cast<std::size_t>(state.range(0));
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_largest_N_Directories(tree, metrics, n));
}
BENCHMARK(BM_LargestNDirectories)->Arg(10)->Arg(50)->Arg(200);

// ---------------------------------------------------------------------------
// Analyzer — compute_extension_stats (hash map + sort)
// ---------------------------------------------------------------------------

static void BM_ExtensionStats(benchmark::State& state) {
    auto tree = make_synthetic_tree(static_cast<std::size_t>(state.range(0)), 50);
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_extension_stats(tree));
}
BENCHMARK(BM_ExtensionStats)->Arg(50)->Arg(200)->Arg(1000);

// ---------------------------------------------------------------------------
// Analyzer — compute_summary
// ---------------------------------------------------------------------------

static void BM_Summary(benchmark::State& state) {
    auto tree    = make_synthetic_tree(200, 50);
    auto metrics = compute_directory_metrics(tree);
    auto empty   = compute_empty_directories(tree);
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_summary(tree, metrics, empty.size()));
}
BENCHMARK(BM_Summary);

// ---------------------------------------------------------------------------
// Analyzer — compute_recent_files (filter + sort)
// ---------------------------------------------------------------------------

static void BM_RecentFiles(benchmark::State& state) {
    auto tree = make_synthetic_tree(200, 50);
    auto window = std::chrono::seconds{60 * 60 * 24 * 7}; // 7 days
    for (auto _ : state)
        benchmark::DoNotOptimize(compute_recent_files(tree, window));
}
BENCHMARK(BM_RecentFiles);

// ---------------------------------------------------------------------------
// View — format_size
// ---------------------------------------------------------------------------

static void BM_FormatSize(benchmark::State& state) {
    std::uint64_t bytes = static_cast<std::uint64_t>(state.range(0));
    for (auto _ : state)
        benchmark::DoNotOptimize(format_size(bytes));
}
BENCHMARK(BM_FormatSize)
    ->Arg(512)
    ->Arg(1024 * 1024)
    ->Arg(1024LL * 1024 * 1024 * 3);

// ---------------------------------------------------------------------------
// Builder — build_tree (real I/O)
// Creates a small controlled directory tree so the benchmark stays bounded.
// ---------------------------------------------------------------------------

class BuildTreeFixture : public benchmark::Fixture {
public:
    std::filesystem::path root;

    void SetUp(benchmark::State&) override {
        root = std::filesystem::temp_directory_path() / "phanes_bench_tree";
        std::filesystem::remove_all(root);
        for (int d = 0; d < 20; ++d) {
            auto dir = root / ("dir" + std::to_string(d));
            std::filesystem::create_directories(dir);
            for (int f = 0; f < 50; ++f) {
                std::ofstream{dir / ("file" + std::to_string(f) + ".txt")} << "x";
            }
        }
    }

    void TearDown(benchmark::State&) override {
        std::filesystem::remove_all(root);
    }
};

BENCHMARK_DEFINE_F(BuildTreeFixture, BM_BuildTree)(benchmark::State& state) {
    for (auto _ : state)
        benchmark::DoNotOptimize(build_tree(root));
}
BENCHMARK_REGISTER_F(BuildTreeFixture, BM_BuildTree)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(10);

BENCHMARK_MAIN();
