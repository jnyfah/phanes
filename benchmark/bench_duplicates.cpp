#include <benchmark/benchmark.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <vector>

import core;
import builder;
import analyzer;

namespace fs = std::filesystem;

// ============================================================================
// Fixture helpers
// ============================================================================

namespace
{

fs::path make_bench_path(std::string_view stem)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           std::format("{}_{}_{}", stem, now, std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void create_fixture(const fs::path& root,
                    int num_groups,
                    int copies_per_group,
                    std::size_t file_size,
                    int num_unique = 0)
{
    fs::remove_all(root);
    fs::create_directories(root);

    std::vector<char> buf(file_size);

    for (int g = 0; g < num_groups; ++g)
    {
        for (std::size_t i = 0; i < file_size; ++i)
            buf[i] = static_cast<char>((g * 7 + i) & 0xFF);

        for (int c = 0; c < copies_per_group; ++c)
        {
            std::ofstream f(root / std::format("g{}_c{}.bin", g, c), std::ios::binary);
            f.write(buf.data(), static_cast<std::streamsize>(file_size));
        }
    }

    // unique files: same size but distinct content — stage 1 should eliminate them
    for (int u = 0; u < num_unique; ++u)
    {
        for (std::size_t i = 0; i < file_size; ++i)
            buf[i] = static_cast<char>((num_groups * 7 + u * 13 + i) & 0xFF);
        std::ofstream f(root / std::format("unique{}.bin", u), std::ios::binary);
        f.write(buf.data(), static_cast<std::streamsize>(file_size));
    }
}

} // namespace

// ============================================================================
// Thread scaling
// ============================================================================

static void BM_Duplicates_ThreadScaling(benchmark::State& state)
{
    const auto num_threads = static_cast<std::size_t>(state.range(0));
    const auto root = make_bench_path("phanes_dup_threads");

    constexpr int groups = 20;
    constexpr int copies = 4;
    constexpr std::size_t file_size = 512 * 1024; // 512KB

    create_fixture(root, groups, copies, file_size);
    auto tree = build_tree(root);

    const int64_t total_files = groups * copies;
    const int64_t total_bytes = total_files * static_cast<int64_t>(file_size);

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_duplicate_groups(tree, num_threads));

    state.SetItemsProcessed(state.iterations() * total_files);
    state.SetBytesProcessed(state.iterations() * total_bytes);
    state.SetLabel(std::format("{} thread(s)", num_threads));

    fs::remove_all(root);
}
BENCHMARK(BM_Duplicates_ThreadScaling)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Unit(benchmark::kMillisecond);

// ============================================================================
// File size scaling
// ============================================================================

static void BM_Duplicates_FileSizeScaling(benchmark::State& state)
{
    const auto file_size = static_cast<std::size_t>(state.range(0));
    const auto root = make_bench_path("phanes_dup_filesize");

    constexpr int groups = 8;
    constexpr int copies = 4;

    create_fixture(root, groups, copies, file_size);
    auto tree = build_tree(root);

    const int64_t total_bytes = groups * copies * static_cast<int64_t>(file_size);

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_duplicate_groups(tree, 0)); // 0 = hw_concurrency

    state.SetBytesProcessed(state.iterations() * total_bytes);
    state.SetLabel(std::format("{}KB/file", file_size / 1024));

    fs::remove_all(root);
}
BENCHMARK(BM_Duplicates_FileSizeScaling)
    ->Arg(1 * 1024)          //   1KB — tiny fast path
    ->Arg(4 * 1024)          //   4KB — boundary of tiny fast path
    ->Arg(16 * 1024)         //  16KB — front+end samples, then full read
    ->Arg(64 * 1024)         //  64KB — two-stage filter clearly engaged
    ->Arg(256 * 1024)        // 256KB
    ->Arg(1024 * 1024)       //   1MB — full hash dominates
    ->Arg(4 * 1024 * 1024)   //   4MB
    ->Unit(benchmark::kMillisecond);

// ============================================================================
// Filter effectiveness
// ============================================================================
//
static void BM_Duplicates_FilterEffectiveness(benchmark::State& state)
{
    const auto num_unique = static_cast<int>(state.range(0));
    const auto root = make_bench_path("phanes_dup_filter");

    constexpr int groups = 5;
    constexpr int copies = 4;
    constexpr std::size_t file_size = 256 * 1024; // 256KB — large enough to make full reads expensive

    create_fixture(root, groups, copies, file_size, num_unique);
    auto tree = build_tree(root);

    const int total_files = groups * copies + num_unique;

    for (auto _ : state)
        benchmark::DoNotOptimize(compute_duplicate_groups(tree, 0));

    state.SetItemsProcessed(state.iterations() * total_files);
    state.SetLabel(std::format("{} unique / {} total", num_unique, total_files));

    fs::remove_all(root);
}
BENCHMARK(BM_Duplicates_FilterEffectiveness)
    ->Arg(0)   // baseline: all files are duplicates, nothing for filter to prune
    ->Arg(10)
    ->Arg(50)
    ->Arg(100) // 100 unique files: filter should eliminate them cheaply at stage 1
    ->Arg(500)
    ->Unit(benchmark::kMillisecond);
