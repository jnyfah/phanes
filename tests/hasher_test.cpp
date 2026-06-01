#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

import analyzer;
import phanes_hasher;

// ============================================================
// Helper — hash a single buffer in one shot using the streaming API
// ============================================================

static uint64_t hash_once(const uint8_t* data, size_t len)
{
    PhanesHashState state;
    phanes_hash_reset(state);
    phanes_hash_update(state, data, len);
    return phanes_hash_digest(state);
}

static void fill(uint8_t* buf, size_t len, uint8_t val)
{
    memset(buf, val, len);
}

// ============================================================
// Determinism — same input must always produce the same output
// ============================================================

TEST(PhanesHash, Determinism)
{
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT_EQ(hash_once(data, sizeof(data)), hash_once(data, sizeof(data)));
}

TEST(PhanesHash, EmptyInputIsDeterministic)
{
    const uint8_t data[1] = {0};
    EXPECT_EQ(hash_once(data, 0), hash_once(data, 0));
}

// ============================================================
// Content sensitivity — different bytes → different hash
// ============================================================

TEST(PhanesHash, DifferentContentDifferentHash)
{
    const uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 9};
    EXPECT_NE(hash_once(a, 8), hash_once(b, 8));
}

TEST(PhanesHash, SingleBitFlipChangesHash)
{
    uint8_t a[32], b[32];
    fill(a, 32, 0xAB);
    fill(b, 32, 0xAB);
    b[16] ^= 0x01;
    EXPECT_NE(hash_once(a, 32), hash_once(b, 32));
}

TEST(PhanesHash, AllZerosNotSameAsAllOnes)
{
    uint8_t zeros[64] = {};
    uint8_t ones[64];
    fill(ones, 64, 0xFF);
    EXPECT_NE(hash_once(zeros, 64), hash_once(ones, 64));
}

// ============================================================
// Length sensitivity
// ============================================================

TEST(PhanesHash, LongerInputDifferentHash)
{
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    EXPECT_NE(hash_once(data, 8), hash_once(data, 9));
}

TEST(PhanesHash, EmptyNotSameAsOneByte)
{
    const uint8_t data[] = {0xAA};
    EXPECT_NE(hash_once(data, 0), hash_once(data, 1));
}

// ============================================================
// Order sensitivity
// ============================================================

TEST(PhanesHash, OrderMatters)
{
    const uint8_t ab[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t ba[] = {9, 10, 11, 12, 13, 14, 15, 16, 1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_NE(hash_once(ab, 16), hash_once(ba, 16));
}

// ============================================================
// Tail sensitivity
// ============================================================

TEST(PhanesHash, TailByteChangesHash)
{
    const uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xAA};
    const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xBB};
    EXPECT_NE(hash_once(a, 9), hash_once(b, 9));
}

TEST(PhanesHash, TailPositionMatters)
{
    const uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0x00};
    const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 8, 0x00, 0xFF};
    EXPECT_NE(hash_once(a, 10), hash_once(b, 10));
}

// ============================================================
// Streaming consistency — one update vs many updates of the same
// data must produce the same hash.
// This is the key property of the streaming API.
// ============================================================

TEST(PhanesHash, StreamingMatchesSingleCall)
{
    // 100 bytes: feed as one call, then as 4 separate calls
    uint8_t data[100];
    for (int i = 0; i < 100; i++)
        data[i] = static_cast<uint8_t>(i);

    uint64_t single = hash_once(data, 100);

    PhanesHashState state;
    phanes_hash_reset(state);
    phanes_hash_update(state, data,      25);
    phanes_hash_update(state, data + 25, 25);
    phanes_hash_update(state, data + 50, 25);
    phanes_hash_update(state, data + 75, 25);
    uint64_t streamed = phanes_hash_digest(state);

    EXPECT_EQ(single, streamed);
}

TEST(PhanesHash, StreamingArbitraryChunks)
{
    // same data, different chunk boundaries
    uint8_t data[96];
    for (int i = 0; i < 96; i++)
        data[i] = static_cast<uint8_t>(i * 3);

    uint64_t single = hash_once(data, 96);

    // chunk sizes that cross 32-byte block boundaries
    PhanesHashState state;
    phanes_hash_reset(state);
    phanes_hash_update(state, data,      7);
    phanes_hash_update(state, data + 7,  33);
    phanes_hash_update(state, data + 40, 56);
    uint64_t streamed = phanes_hash_digest(state);

    EXPECT_EQ(single, streamed);
}
