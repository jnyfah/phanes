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
    __m256i acc;
    uint8_t buffer[32];
    size_t buf_used;
    size_t total_len;
};

static inline auto rotate_left(__m256i acc, int n) -> __m256i
{
    return _mm256_or_si256(_mm256_slli_epi64(acc, n), _mm256_srli_epi64(acc, 64 - n));
}

static inline auto rotate_left(uint64_t x, int n) -> uint64_t
{
    return (x << n) | (x >> (64 - n));
}

// reset(state)              ← set acc[0..3] to initial seed values
// update(state, data, len)  ← process the data, fill accumulators
// digest(state)             ← merge acc[0..3] into one 64-bit result

export void phanes_hash_reset(PhanesHashState& state)
{
    state.acc = _mm256_set_epi64x(seed - PRIME_1, // lane 3 (acc[3])
                                  seed, // lane 2 (acc[2])
                                  seed + PRIME_2, // lane 1 (acc[1])
                                  seed + PRIME_1 + PRIME_2 // lane 0 (acc[0])
    );

    state.buf_used = 0;
    state.total_len = 0;
    std::memset(state.buffer, 0, sizeof(state.buffer));
}

static inline __m256i mul64(__m256i v, uint64_t c)
{
    __m256i mask32 = _mm256_set1_epi64x(0xFFFFFFFF);

    __m256i v_lo = _mm256_and_si256(v, mask32);
    __m256i v_hi = _mm256_srli_epi64(v, 32);

    __m256i c_lo = _mm256_set1_epi64x((int64_t)(uint32_t)c);
    __m256i c_hi = _mm256_set1_epi64x((int64_t)(c >> 32));

    __m256i ll = _mm256_mul_epu32(v_lo, c_lo);
    __m256i hl = _mm256_mul_epu32(v_hi, c_lo);
    __m256i lh = _mm256_mul_epu32(v_lo, c_hi);

    __m256i cross = _mm256_slli_epi64(_mm256_add_epi64(hl, lh), 32);

    return _mm256_add_epi64(ll, cross);
}

static inline __m256i mix(__m256i acc, __m256i word)
{
    // acc ^= word * PRIME_1;
    acc = _mm256_xor_si256(acc, mul64(word, PRIME_1));
    acc = rotate_left(acc, 31);

    // acc *= PRIME_2;
    acc = mul64(acc, PRIME_2);
    return acc;
}

export void phanes_hash_update(PhanesHashState& state, const uint8_t* data, size_t len)
{
    // If there are leftover bytes from a previous update call sitting in state.buffer

    state.total_len += len; // track total bytes seen across all update calls

    if (state.buf_used > 0)
    {
        auto remaining = 32 - state.buf_used;

        if (remaining > len)
        {
            // copy all of data into buffer used and return
            std::memcpy(state.buffer + state.buf_used, data, len);
            state.buf_used += len;
            return;
        }
        std::memcpy(state.buffer + state.buf_used, data, remaining);

        __m256i word = _mm256_loadu_si256((const __m256i*)state.buffer);

        state.acc = mix(state.acc, word);

        state.buf_used = 0; // buffer is consumed
        data += remaining; // advance past the bytes you already used
        len -= remaining;
    }

    __m256i vacc = state.acc;

    for (size_t i = 0; i + 32 <= len; i += 32)
    {

        __m256i word = _mm256_loadu_si256((const __m256i*)(data + i));
        vacc = mix(vacc, word);
    }

    state.acc = vacc; // store once

    size_t remaining = len % 32;
    std::memcpy(state.buffer, data + (len - remaining), remaining);
    state.buf_used = remaining;
}

export auto phanes_hash_digest(PhanesHashState& state) -> uint64_t
{
    alignas(32) uint64_t lanes[4];
    _mm256_store_si256((__m256i*)lanes, state.acc);

    uint64_t h64 =
        rotate_left(lanes[0], 1) + rotate_left(lanes[1], 7) + rotate_left(lanes[2], 12) + rotate_left(lanes[3], 18);

    h64 += state.total_len;

    if (state.buf_used > 0)
    {
        alignas(32) uint8_t tmp[32] = {};
        std::memcpy(tmp, state.buffer, state.buf_used);
        __m256i tail = _mm256_load_si256((const __m256i*)tmp);
        // extract and fold each lane individually
        alignas(32) uint64_t words[4];
        _mm256_store_si256((__m256i*)words, tail);
        for (unsigned long word : words)
        {
            h64 ^= rotate_left(word, 11) * PRIME_3;
        }
    }

    h64 ^= h64 >> 33;
    h64 *= PRIME_3;
    h64 ^= h64 >> 29;

    return h64;
}