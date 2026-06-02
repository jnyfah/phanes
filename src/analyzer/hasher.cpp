module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

export module phanes_hasher;

static constexpr uint64_t PRIME_1 = 0x9E3779B185EBCA87ULL;
static constexpr uint64_t PRIME_2 = 0xC2B2AE3D27D4EB4FULL;
static constexpr uint64_t PRIME_3 = 0x165667B19E3779F9ULL;
static constexpr uint64_t seed = 0;

export struct PhanesHashState
{
    __m256i acc[4];
    uint8_t buffer[32];
    size_t buf_used;
    size_t total_len;
    size_t blocks;
};

// SIMD
static inline auto rotate_left(__m256i acc, int n) -> __m256i
{
    return _mm256_or_si256(_mm256_slli_epi64(acc, n), _mm256_srli_epi64(acc, 64 - n));
}

// Scalar
static inline auto rotate_left(uint64_t x, int n) -> uint64_t
{
    return (x << n) | (x >> (64 - n));
}

// reset to default values 
export void phanes_hash_reset(PhanesHashState& state)
{

    state.acc[0] = _mm256_set_epi64x(seed - PRIME_1, seed, seed + PRIME_2, seed + PRIME_1 + PRIME_2);
    state.acc[1] = _mm256_set_epi64x(seed + PRIME_3, seed - PRIME_2, seed + PRIME_1, seed + PRIME_3 + PRIME_1);
    state.acc[2] = _mm256_set_epi64x(seed + PRIME_1, seed + PRIME_3, seed - PRIME_2, seed + PRIME_2 + PRIME_3);
    state.acc[3] = _mm256_set_epi64x(seed - PRIME_3, seed + PRIME_1 + PRIME_3, seed + PRIME_2, seed - PRIME_2);
    state.buf_used = 0;
    state.total_len = 0;

    state.blocks = 0;

    std::memset(state.buffer, 0, sizeof(state.buffer));
}

static inline auto mul64(__m256i acc, __m256i c_lo, __m256i c_hi) -> __m256i
{
    // mask to split 64 bits into 2
    const __m256i mask32 = _mm256_set1_epi64x(0xFFFFFFFF);

    // get lower 32 bits
    __m256i v_lo = _mm256_and_si256(acc, mask32);
    // shift upper 32 bits to the right
    __m256i v_hi = _mm256_srli_epi64(acc, 32);

    // multiply
    __m256i ll = _mm256_mul_epu32(v_lo, c_lo);
    __m256i hl = _mm256_mul_epu32(v_hi, c_lo);
    __m256i lh = _mm256_mul_epu32(v_lo, c_hi);

    __m256i cross = _mm256_slli_epi64(_mm256_add_epi64(hl, lh), 32);

    return _mm256_add_epi64(ll, cross);
}

static inline auto mix(__m256i acc, __m256i word, __m256i p1_lo, __m256i p1_hi, __m256i p2_lo, __m256i p2_hi)-> __m256i
{
    // acc ^= word * prime_1
    acc = _mm256_xor_si256(acc, mul64(word, p1_lo, p1_hi));
    acc = rotate_left(acc, 31);

    // acc = acc * prime_2
    acc = mul64(acc, p2_lo, p2_hi);
    return acc;
}

export void phanes_hash_update(PhanesHashState& state, const uint8_t* data, size_t len)
{
    state.total_len += len;

    // split prime constants to 32 bits lower
    const __m256i p1_lo = _mm256_set1_epi64x((int64_t)(uint32_t)PRIME_1);
    const __m256i p1_hi = _mm256_set1_epi64x((int64_t)(PRIME_1 >> 32));
    const __m256i p2_lo = _mm256_set1_epi64x((int64_t)(uint32_t)PRIME_2);
    const __m256i p2_hi = _mm256_set1_epi64x((int64_t)(PRIME_2 >> 32));

    // load 
    __m256i v0 = state.acc[0];
    __m256i v1 = state.acc[1];
    __m256i v2 = state.acc[2];
    __m256i v3 = state.acc[3];

    size_t blocks = state.blocks;

    auto fold = [&](size_t idx, __m256i w)
    {
        switch (idx & 3)
        {
        case 0:
            v0 = mix(v0, w, p1_lo, p1_hi, p2_lo, p2_hi);
            break;
        case 1:
            v1 = mix(v1, w, p1_lo, p1_hi, p2_lo, p2_hi);
            break;
        case 2:
            v2 = mix(v2, w, p1_lo, p1_hi, p2_lo, p2_hi);
            break;
        case 3:
            v3 = mix(v3, w, p1_lo, p1_hi, p2_lo, p2_hi);
            break;
        }
    };

    // flush any pending buffered block first
    if (state.buf_used > 0)
    {
        size_t remaining = 32 - state.buf_used;

        if (remaining > len)
        {
            std::memcpy(state.buffer + state.buf_used, data, len);
            state.buf_used += len;
            return;
        }
        std::memcpy(state.buffer + state.buf_used, data, remaining);

        fold(blocks, _mm256_loadu_si256((const __m256i*)state.buffer));

        ++blocks;

        state.buf_used = 0;
        data += remaining;

        len -= remaining;
    }
    size_t i = 0;
    // realign so the 4x loop starts on a block index that's a multiple of 4

    while ((blocks & 3) != 0 && i + 32 <= len)
    {
        fold(blocks, _mm256_loadu_si256((const __m256i*)(data + i)));

        ++blocks;

        i += 32;
    }

    for (; i + 128 <= len; i += 128)
    {
        __m256i w0 = _mm256_loadu_si256((const __m256i*)(data + i));

        __m256i w1 = _mm256_loadu_si256((const __m256i*)(data + i + 32));

        __m256i w2 = _mm256_loadu_si256((const __m256i*)(data + i + 64));

        __m256i w3 = _mm256_loadu_si256((const __m256i*)(data + i + 96));

        v0 = mix(v0, w0, p1_lo, p1_hi, p2_lo, p2_hi);

        v1 = mix(v1, w1, p1_lo, p1_hi, p2_lo, p2_hi);

        v2 = mix(v2, w2, p1_lo, p1_hi, p2_lo, p2_hi);

        v3 = mix(v3, w3, p1_lo, p1_hi, p2_lo, p2_hi);
        blocks += 4;
    }

    while (i + 32 <= len)
    {
        fold(blocks, _mm256_loadu_si256((const __m256i*)(data + i)));
        ++blocks;
        i += 32;
    }

    // store
    state.acc[0] = v0;
    state.acc[1] = v1;
    state.acc[2] = v2;
    state.acc[3] = v3;
    state.blocks = blocks;

    // copy the overflow into buffer
    size_t remaining = len - i;
    std::memcpy(state.buffer, data + i, remaining);
    state.buf_used = remaining;
}

export auto phanes_hash_digest(PhanesHashState& state) -> uint64_t
{
    alignas(32) uint64_t lanes[16];
    _mm256_store_si256((__m256i*)(lanes + 0), state.acc[0]);

    _mm256_store_si256((__m256i*)(lanes + 4), state.acc[1]);

    _mm256_store_si256((__m256i*)(lanes + 8), state.acc[2]);

    _mm256_store_si256((__m256i*)(lanes + 12), state.acc[3]);

    // fold all 16 lanes — rotate each by a different amount so they don't cancel

    static constexpr int rots[16] = {1, 7, 12, 18, 23, 27, 31, 36, 41, 45, 50, 54, 2, 9, 15, 20};

    uint64_t h64 = 0;

    for (int j = 0; j < 16; ++j)
    {
        h64 += rotate_left(lanes[j], rots[j]);
    }

    h64 += state.total_len;

    for (size_t i = 0; i < state.buf_used; i++)
    {
        h64 ^= (uint64_t)state.buffer[i] * PRIME_3;
        h64 = rotate_left(h64, 11) * PRIME_1;
    }

    h64 ^= h64 >> 33;
    h64 *= PRIME_3;
    h64 ^= h64 >> 29;

    return h64;
}