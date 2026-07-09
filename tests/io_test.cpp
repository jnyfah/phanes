#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

import phanes_io;


namespace fs = std::filesystem;

namespace
{

struct TempDir
{
    fs::path path;

    TempDir()
    {
        std::random_device rd;
        std::uniform_int_distribution<std::uint64_t> dist;
        do
        {
            path = fs::temp_directory_path() / std::format("phanes_io_test_{:016x}", dist(rd));
        } while (!fs::create_directory(path));
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write(const std::string& name, std::string_view content) const
    {
        auto full = path / name;
        std::ofstream(full, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
        return full;
    }
};

// Deterministic pseudo-random bytes, so content comparisons are meaningful.
std::string make_content(std::size_t n, std::uint64_t seed)
{
    std::string s(n, '\0');
    std::uint64_t x = seed | 1;
    for (auto& c : s)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        c = static_cast<char>(x & 0xFF);
    }
    return s;
}

// The first `res` valid bytes of a completed read's buffer, as text for comparison.
std::string_view first(const std::vector<std::byte>& buf, int res)
{
    return {reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(res)};
}

// io_uring can be blocked by seccomp in some CI sandboxes; skip rather than fail there.
#define MAKE_RING_OR_SKIP(ring)                                                                                        \
    Ring ring;                                                                                                         \
    if (!ring.init(64).has_value())                                                                                    \
    {                                                                                                                  \
        GTEST_SKIP() << "async I/O ring unavailable in this environment";                                             \
    }                                                                                                                  \
    static_assert(true)

} // namespace

TEST(PhanesIoRing, ReadsWholeFile)
{
    TempDir dir;
    const std::string content = "hello async ring";
    const auto p = dir.write("a.txt", content);

    MAKE_RING_OR_SKIP(ring);

    auto tag = ring.submit(p, content.size(), 0);
    ASSERT_TRUE(tag.has_value());

    auto res = ring.next();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->tag, *tag);
    ASSERT_EQ(res->res, static_cast<int>(content.size()));
    EXPECT_EQ(first(ring.data(res->tag), res->res), content);
}

TEST(PhanesIoRing, ReadsFromOffset)
{
    TempDir dir;
    const std::string content = make_content(8192, 0xABCD);
    const auto p = dir.write("b.bin", content);

    MAKE_RING_OR_SKIP(ring);

    constexpr std::size_t off = 4096;
    constexpr std::size_t len = 2048;
    auto tag = ring.submit(p, len, static_cast<std::int64_t>(off));
    ASSERT_TRUE(tag.has_value());

    auto res = ring.next();
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->res, static_cast<int>(len));
    EXPECT_EQ(first(ring.data(res->tag), res->res), std::string_view(content).substr(off, len));
}

TEST(PhanesIoRing, PartialReadStopsAtEof)
{
    // Request more than the file holds; the completion reports only the bytes read.
    TempDir dir;
    const std::string content = make_content(1000, 0x11);
    const auto p = dir.write("c.bin", content);

    MAKE_RING_OR_SKIP(ring);

    auto tag = ring.submit(p, 65536, 0);
    ASSERT_TRUE(tag.has_value());

    auto res = ring.next();
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->res, static_cast<int>(content.size()));
    EXPECT_EQ(first(ring.data(res->tag), res->res), content);
}

TEST(PhanesIoRing, MissingFileFails)
{
    TempDir dir;
    MAKE_RING_OR_SKIP(ring);

    auto tag = ring.submit(dir.path / "does_not_exist", 128, 0);
    EXPECT_FALSE(tag.has_value());
}

TEST(PhanesIoRing, ManyReadsCompleteWithCorrectTags)
{
    // Submit reads for many files at once; completions may come back in any order,
    // so each returned tag must still map to exactly the content that was submitted.
    TempDir dir;
    constexpr int N = 40;
    std::unordered_map<std::size_t, std::string> expected; // tag -> content

    MAKE_RING_OR_SKIP(ring);

    int submitted = 0;
    for (int i = 0; i < N; ++i)
    {
        const auto content = make_content(256 + static_cast<std::size_t>(i) * 7, 0x1000ULL + i);
        const auto p = dir.write(std::format("f{}.bin", i), content);
        auto tag = ring.submit(p, content.size(), 0);
        ASSERT_TRUE(tag.has_value());
        expected[*tag] = content;
        ++submitted;
    }

    int seen = 0;
    while (seen < submitted)
    {
        auto res = ring.next();
        ASSERT_TRUE(res.has_value());
        const auto it = expected.find(res->tag);
        ASSERT_NE(it, expected.end());
        ASSERT_EQ(res->res, static_cast<int>(it->second.size()));
        EXPECT_EQ(first(ring.data(res->tag), res->res), it->second);
        ++seen;
    }
    EXPECT_EQ(seen, submitted);
}

TEST(PhanesIoRing, StreamFileInChunksReassembles)
{
    // Mirror hash_file: read a file in fixed chunks by advancing the offset,
    // reassemble, and compare to the original.
    TempDir dir;
    const std::string content = make_content(10000, 0x9);
    const auto p = dir.write("big.bin", content);

    MAKE_RING_OR_SKIP(ring);

    constexpr std::size_t CHUNK = 4096;
    std::string got;
    std::size_t offset = 0;
    while (offset < content.size())
    {
        auto tag = ring.submit(p, CHUNK, static_cast<std::int64_t>(offset));
        ASSERT_TRUE(tag.has_value());

        auto res = ring.next();
        ASSERT_TRUE(res.has_value());
        ASSERT_GT(res->res, 0);
        got.append(first(ring.data(res->tag), res->res));
        ring.release(res->tag);
        offset += static_cast<std::size_t>(res->res);
    }
    EXPECT_EQ(got, content);
}

TEST(PhanesIoRing, ReleaseRecyclesSlotAndResetKeepsRingUsable)
{
    TempDir dir;
    const std::string content = "recycle me";
    const auto p = dir.write("r.txt", content);

    MAKE_RING_OR_SKIP(ring);

    auto t1 = ring.submit(p, content.size(), 0);
    ASSERT_TRUE(t1.has_value());
    auto r1 = ring.next();
    ASSERT_TRUE(r1.has_value());
    ring.release(r1->tag);

    // The freed slot is reused, so the next submit hands back the same tag.
    auto t2 = ring.submit(p, content.size(), 0);
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ(*t2, r1->tag);
    auto r2 = ring.next();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->res, static_cast<int>(content.size()));

    // After reset the ring is still usable for fresh work.
    ring.reset();
    auto t3 = ring.submit(p, content.size(), 0);
    ASSERT_TRUE(t3.has_value());
    auto r3 = ring.next();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->res, static_cast<int>(content.size()));
}
