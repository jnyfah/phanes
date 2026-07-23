#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

import core;
import builder;
import analyzer;
import phanes_io;

namespace fs = std::filesystem;

namespace
{

fs::path bench_root(std::string_view stem)
{
    const char* base = std::getenv("PHANES_BENCH_DIR");
    const fs::path dir = base ? fs::path(base) : fs::temp_directory_path();
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return dir / std::format("phanes_{}_{}", stem, now);
}

int64_t create_fixture(const fs::path& root,
                       int groups,
                       int copies,
                       std::size_t file_size,
                       int unique = 0,
                       std::size_t size_stride = 0)
{
    fs::remove_all(root);
    fs::create_directories(root);

    int64_t total = 0;
    std::vector<char> buf;

    for (int g = 0; g < groups; ++g)
    {
        const std::size_t sz = file_size + static_cast<std::size_t>(g) * size_stride;
        buf.assign(sz, 0);
        for (std::size_t i = 0; i < sz; ++i)
        {
            buf[i] = static_cast<char>((g * 7 + i) & 0xFF);
        }
        for (int c = 0; c < copies; ++c)
        {
            std::ofstream f(root / std::format("g{}_c{}.bin", g, c), std::ios::binary);
            f.write(buf.data(), static_cast<std::streamsize>(sz));
            total += static_cast<int64_t>(sz);
        }
    }

    // unique files keep the BASE size so they pad group 0, which is what the
    // filter benchmark is measuring
    for (int u = 0; u < unique; ++u)
    {
        buf.assign(file_size, 0);
        for (std::size_t i = 0; i < file_size; ++i)
        {
            buf[i] = static_cast<char>((groups * 7 + u * 13 + i) & 0xFF);
        }
        std::ofstream f(root / std::format("unique{}.bin", u), std::ios::binary);
        f.write(buf.data(), static_cast<std::streamsize>(file_size));
        total += static_cast<int64_t>(file_size);
    }

    return total;
}

std::vector<fs::path> list_files(const fs::path& root)
{
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(root))
    {
        if (e.is_regular_file())
        {
            out.push_back(e.path());
        }
    }
    return out;
}

void evict(const std::vector<fs::path>& files)
{
    ::sync();
    for (const auto& p : files)
    {
        int fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0)
        {
            continue;
        }
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
}

std::size_t consume(auto&& gen)
{
    std::size_t n = 0;
    for (auto&& g : gen)
    {
        benchmark::DoNotOptimize(g);
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Fixture geometry. IDENTICAL in bench_ring.cpp -- change both or neither.
// ---------------------------------------------------------------------------
constexpr int kGroups = 64;
constexpr int kCopies = 4;
constexpr std::size_t kFileSize = 1024 * 1024;
constexpr std::size_t kThreadCounts[] = {1, 2, 4, 8, 16, 22, 44};

constexpr std::size_t kSizeStride = 4096;
constexpr unsigned kRingEntries = 1024;

struct Corpus
{
    fs::path root;
    std::vector<fs::path> files;
    decltype(build_tree(fs::path{})) tree;
    int64_t bytes = 0;

    Corpus(std::string_view stem, int groups, int copies, std::size_t size, int unique = 0, std::size_t size_stride = 0)
        : root(bench_root(stem))
    {
        bytes = create_fixture(root, groups, copies, size, unique, size_stride);
        files = list_files(root);
        tree = build_tree(root);
    }
    ~Corpus() { fs::remove_all(root); }
};

} // namespace

// ============================================================================

int main(int argc, char** argv)
{

    static Corpus scan_corpus("scan", kGroups, kCopies, kFileSize, 0, kSizeStride);
    static Corpus small_corpus("small", 256, 2, 64 * 1024);

    // ---- Scan: warm ----
    for (std::size_t t : kThreadCounts)
    {
        benchmark::RegisterBenchmark(std::format("Scan/warm/{}t", t),
                                     [t](benchmark::State& st)
                                     {
                                         for (auto _ : st)
                                         {
                                             auto gen = compute_duplicate_groups(scan_corpus.tree, t);
                                             consume(gen);
                                         }
                                         st.SetBytesProcessed(st.iterations() * scan_corpus.bytes);
                                         st.SetItemsProcessed(st.iterations() *
                                                              static_cast<int64_t>(scan_corpus.files.size()));
                                     })
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);
    }

    for (std::size_t t : kThreadCounts)
    {
        benchmark::RegisterBenchmark(std::format("Scan/cold/{}t", t),
                                     [t](benchmark::State& st)
                                     {
                                         for (auto _ : st)
                                         {
                                             evict(scan_corpus.files);
                                             const auto t0 = std::chrono::steady_clock::now();
                                             auto gen = compute_duplicate_groups(scan_corpus.tree, t);
                                             consume(gen);
                                             const auto t1 = std::chrono::steady_clock::now();
                                             st.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
                                         }
                                         st.SetBytesProcessed(st.iterations() * scan_corpus.bytes);
                                     })
            ->UseManualTime()
            ->Iterations(1)
            ->Repetitions(5)
            ->DisplayAggregatesOnly()
            ->Unit(benchmark::kMillisecond);
    }

    // ---- File size scaling ----
    for (std::size_t sz : {std::size_t{1024},
                           std::size_t{4096},
                           std::size_t{16384},
                           std::size_t{65536},
                           std::size_t{262144},
                           std::size_t{1048576},
                           std::size_t{4194304}})
    {
        benchmark::RegisterBenchmark(std::format("FileSize/{}KB", sz / 1024),
                                     [sz](benchmark::State& st)
                                     {
                                         Corpus c("filesize", 8, 4, sz);
                                         for (auto _ : st)
                                         {
                                             auto gen = compute_duplicate_groups(c.tree, 0);
                                             consume(gen);
                                         }
                                         st.SetBytesProcessed(st.iterations() * c.bytes);
                                     })
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);
    }

    // ---- Filter effectiveness ----
    for (int u : {0, 10, 50, 100, 500})
    {
        benchmark::RegisterBenchmark(std::format("Filter/{}unique", u),
                                     [u](benchmark::State& st)
                                     {
                                         Corpus c("filter", 5, 4, 256 * 1024, u);
                                         for (auto _ : st)
                                         {
                                             auto gen = compute_duplicate_groups(c.tree, 0);
                                             consume(gen);
                                         }
                                         st.SetItemsProcessed(st.iterations() * static_cast<int64_t>(c.files.size()));
                                     })
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);
    }

    benchmark::RegisterBenchmark("PerFile/open",
                                 [](benchmark::State& st)
                                 {
                                     for (auto _ : st)
                                     {
                                         for (const auto& p : small_corpus.files)
                                         {
                                             int fd = ::open(p.c_str(), O_RDONLY);
                                             benchmark::DoNotOptimize(fd);
                                             if (fd >= 0)
                                             {
                                                 ::close(fd);
                                             }
                                         }
                                     }
                                     st.SetItemsProcessed(st.iterations() *
                                                          static_cast<int64_t>(small_corpus.files.size()));
                                 })
        ->UseRealTime()
        ->Unit(benchmark::kMillisecond);

    benchmark::RegisterBenchmark(
        "PerFile/prefilter",
        [](benchmark::State& st)
        {
            std::vector<char> buf(4096);
            constexpr std::size_t fsz = 64 * 1024;
            for (auto _ : st)
            {
                for (const auto& p : small_corpus.files)
                {
                    int fd = ::open(p.c_str(), O_RDONLY);
                    if (fd < 0)
                    {
                        continue;
                    }
                    const off_t offs[3] = {0, static_cast<off_t>(fsz / 2 - 2048), static_cast<off_t>(fsz - 4096)};
                    for (off_t o : offs)
                    {
                        auto n = ::pread(fd, buf.data(), buf.size(), o);
                        benchmark::DoNotOptimize(n);
                    }
                    ::close(fd);
                }
            }
            const auto n = st.iterations() * static_cast<int64_t>(small_corpus.files.size());
            st.SetItemsProcessed(n);
            st.SetBytesProcessed(n * 3 * 4096);
        })
        ->UseRealTime()
        ->Unit(benchmark::kMillisecond);

    // Blocking-pread origin for the comparison. Without it you have two points
    // and no baseline -- if both backends land here, neither ring is helping.
    benchmark::RegisterBenchmark("PerFile/fullread",
                                 [](benchmark::State& st)
                                 {
                                     std::vector<char> buf(4 * 1024 * 1024);
                                     int64_t bytes = 0;
                                     for (auto _ : st)
                                     {
                                         for (const auto& p : scan_corpus.files)
                                         {
                                             int fd = ::open(p.c_str(), O_RDONLY);
                                             if (fd < 0)
                                             {
                                                 continue;
                                             }
                                             off_t off = 0;
                                             for (;;)
                                             {
                                                 auto n = ::pread(fd, buf.data(), buf.size(), off);
                                                 if (n <= 0)
                                                 {
                                                     break;
                                                 }
                                                 benchmark::DoNotOptimize(buf.data());
                                                 off += n;
                                                 bytes += n;
                                             }
                                             ::close(fd);
                                         }
                                     }
                                     st.SetBytesProcessed(bytes);
                                 })
        ->UseRealTime()
        ->Unit(benchmark::kMillisecond);

    for (unsigned depth : {1u, 4u, 16u, 64u, 256u})
    {
        benchmark::RegisterBenchmark(std::format("Ring/depth/{}", depth),
                                     [depth](benchmark::State& st)
                                     {
                                         constexpr std::size_t chunk = 256 * 1024;
                                         int64_t bytes = 0;
                                         for (auto _ : st)
                                         {
                                             evict(scan_corpus.files);
                                             const auto t0 = std::chrono::steady_clock::now();

                                             Ring ring;
                                             if (!ring.init(depth))
                                             {
                                                 st.SkipWithError("ring init failed");
                                                 break;
                                             }

                                             std::size_t next = 0;
                                             unsigned outstanding = 0;
                                             while (outstanding < depth && next < scan_corpus.files.size())
                                             {
                                                 if (ring.submit(scan_corpus.files[next], chunk, 0))
                                                 {
                                                     ++outstanding;
                                                 }
                                                 ++next;
                                             }
                                             while (outstanding > 0)
                                             {
                                                 auto r = ring.next();
                                                 if (!r)
                                                 {
                                                     break;
                                                 }
                                                 if (r->res > 0)
                                                 {
                                                     bytes += r->res;
                                                 }
                                                 ring.release(r->tag);
                                                 --outstanding;
                                                 if (next < scan_corpus.files.size())
                                                 {
                                                     if (ring.submit(scan_corpus.files[next], chunk, 0))
                                                     {
                                                         ++outstanding;
                                                     }
                                                     ++next;
                                                 }
                                             }

                                             const auto t1 = std::chrono::steady_clock::now();
                                             st.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
                                         }
                                         st.SetBytesProcessed(bytes);
                                     })
            ->UseManualTime()
            ->Iterations(1)
            ->Repetitions(5)
            ->DisplayAggregatesOnly()
            ->Unit(benchmark::kMillisecond);
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
    {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}