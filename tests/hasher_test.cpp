#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

import analyzer;

// ============================================================
// Helpers
// ============================================================

// fill a buffer with a repeating pattern so tests are readable
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
    EXPECT_EQ(phanes_hash(data, sizeof(data)), phanes_hash(data, sizeof(data)));
}

TEST(PhanesHash, EmptyInputIsDeterministic)
{
    const uint8_t data[1] = {0};
    EXPECT_EQ(phanes_hash(data, 0), phanes_hash(data, 0));
}

// ============================================================
// Content sensitivity — different bytes → different hash
// ============================================================

TEST(PhanesHash, DifferentContentDifferentHash)
{
    const uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 9}; // last byte differs
    EXPECT_NE(phanes_hash(a, 8), phanes_hash(b, 8));
}

TEST(PhanesHash, SingleBitFlipChangesHash)
{
    uint8_t a[32], b[32];
    fill(a, 32, 0xAB);
    fill(b, 32, 0xAB);
    b[16] ^= 0x01; // flip one bit in the middle
    EXPECT_NE(phanes_hash(a, 32), phanes_hash(b, 32));
}

TEST(PhanesHash, AllZerosNotSameAsAllOnes)
{
    uint8_t zeros[64] = {};
    uint8_t ones[64];
    fill(ones, 64, 0xFF);
    EXPECT_NE(phanes_hash(zeros, 64), phanes_hash(ones, 64));
}

// ============================================================
// Length sensitivity — same bytes, different length → different hash
// ============================================================

TEST(PhanesHash, LongerInputDifferentHash)
{
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    EXPECT_NE(phanes_hash(data, 8), phanes_hash(data, 9));
}

TEST(PhanesHash, EmptyNotSameAsOneByte)
{
    const uint8_t data[] = {0};
    EXPECT_NE(phanes_hash(data, 0), phanes_hash(data, 1));
}

// ============================================================
// Order sensitivity — "ab" must not equal "ba"
// Without rotate, XOR of words would be commutative and this would fail.
// ============================================================

TEST(PhanesHash, OrderMatters)
{
    const uint8_t ab[] = {1, 2, 3, 4, 5, 6, 7, 8,  // word 0
                          9, 10, 11, 12, 13, 14, 15, 16}; // word 1
    const uint8_t ba[] = {9, 10, 11, 12, 13, 14, 15, 16,  // swapped
                          1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_NE(phanes_hash(ab, 16), phanes_hash(ba, 16));
}

// ============================================================
// Tail sensitivity — changing a byte past the last full 8-byte
// word must change the hash.
//
// NOTE: if this fails, the tail bytes are being ignored entirely.
// If it passes but TailOrderMatters fails, tail is included but
// not mixed properly (bytes placed identically regardless of position).
// ============================================================

TEST(PhanesHash, TailByteChangesHash)
{
    // 9 bytes: 8 full + 1 tail
    const uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xAA};
    const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xBB};
    EXPECT_NE(phanes_hash(a, 9), phanes_hash(b, 9));
}

TEST(PhanesHash, TailPositionMatters)
{
    // same bytes in the tail but at different positions
    const uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0x00};
    const uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 8, 0x00, 0xFF};
    EXPECT_NE(phanes_hash(a, 10), phanes_hash(b, 10));
}

// ============================================================
// Finalization order check — tail bytes processed BEFORE finalization
// means the finalizer mixes them in. If tail comes AFTER finalization,
// the test below can still pass but the hash quality is poor
// (tail bytes land in the output almost raw, with no diffusion).
//
// There is no pure correctness test for this — it requires an
// avalanche test (statistical). This comment marks where that
// test would live in checkpoint 3.
// ============================================================
