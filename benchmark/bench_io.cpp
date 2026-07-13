#include <benchmark/benchmark.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

import phanes_io;

namespace fs = std::filesystem;

namespace
{

struct Corpus
{
    fs::path dir;
    fs::path big;
    std::vector<fs::path> small;

    Corpus()
    {
        dir = fs::temp_directory_path() / "phanes_bench_io";
        fs::remove_all(dir);
        fs::create_directories(dir);

        big = dir / "big.bin";
        {
            const std::string buf(16u * 1024 * 1024, 'A'); // 16 MB
            std::ofstream(big, std::ios::binary).write(buf.data(), static_cast<std::streamsize>(buf.size()));
        }

        const std::string small_buf(4096, 'x');
        for (int i = 0; i < 512; ++i)
        {
            auto p = dir / ("s" + std::to_string(i) + ".bin");
            std::ofstream(p, std::ios::binary).write(small_buf.data(), static_cast<std::streamsize>(small_buf.size()));
            small.push_back(p);
        }
    }

    ~Corpus()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

const Corpus& corpus()
{
    static const Corpus c;
    return c;
}

} // namespace

static void BM_Ring_ChunkedRead(benchmark::State& state)
{
    const auto& c = corpus();
    constexpr std::int64_t CHUNK = 4LL * 1024 * 1024;
    const auto size = static_cast<std::int64_t>(fs::file_size(c.big));

    Ring ring;
    if (!ring.init(64).has_value())
    {
        state.SkipWithError("async I/O ring unavailable");
        return;
    }

    for (auto _ : state)
    {
        std::int64_t offset = 0;
        while (offset < size)
        {
            auto tag = ring.submit(c.big, static_cast<std::size_t>(CHUNK), offset);
            if (!tag)
            {
                break;
            }
            auto res = ring.next();
            if (!res || res->res <= 0)
            {
                break;
            }
            benchmark::DoNotOptimize(ring.data(res->tag).data());
            ring.release(res->tag);
            offset += res->res;
        }
        ring.reset();
    }
    state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_Ring_ChunkedRead)->Unit(benchmark::kMicrosecond);

static void BM_Ring_ManyFilesWindowed(benchmark::State& state)
{
    const auto& c = corpus();
    const int window = static_cast<int>(state.range(0));
    constexpr std::size_t LEN = 4096;

    Ring ring;
    if (!ring.init(1024).has_value())
    {
        state.SkipWithError("async I/O ring unavailable");
        return;
    }

    for (auto _ : state)
    {
        std::size_t next = 0;
        int in_flight = 0;

        auto pump = [&]
        {
            while (next < c.small.size() && in_flight < window)
            {
                if (ring.submit(c.small[next], LEN, 0))
                {
                    ++in_flight;
                }
                ++next;
            }
        };

        pump();
        while (in_flight > 0)
        {
            auto res = ring.next();
            --in_flight;
            if (res)
            {
                benchmark::DoNotOptimize(ring.data(res->tag).data());
                ring.release(res->tag);
            }
            pump();
        }
        ring.reset();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(c.small.size()));
}
BENCHMARK(BM_Ring_ManyFilesWindowed)->Arg(16)->Arg(64)->Arg(256)->Unit(benchmark::kMicrosecond);
