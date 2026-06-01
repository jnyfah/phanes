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
    uint64_t acc[4];
    uint8_t buffer[32];
    size_t buf_used;
    size_t total_len;
};

static inline auto rotate_left(uint64_t x, int n) -> uint64_t
{
    return (x << n) | (x >> (64 - n));
}

// reset(state)              ← set acc[0..3] to initial seed values
// update(state, data, len)  ← process the data, fill accumulators
// digest(state)             ← merge acc[0..3] into one 64-bit result

export void phanes_hash_reset(PhanesHashState& state)
{
    state.acc[0] = seed + PRIME_1 + PRIME_2;
    state.acc[1] = seed + PRIME_2;
    state.acc[2] = seed;
    state.acc[3] = seed - PRIME_1;

    state.buf_used = 0;
    state.total_len = 0;
    std::memset(state.buffer, 0, sizeof(state.buffer));
}

static inline uint64_t mix(uint64_t acc, uint64_t word)
{
    acc ^= word * PRIME_1;
    acc = rotate_left(acc, 31);
    acc *= PRIME_2;

    return acc;
}

export void phanes_hash_update(PhanesHashState& state, const uint8_t* data, size_t len)
{
    // If there are leftover bytes from a previous update call sitting in state.buffer,
    // you need to fill that buffer first before processing new data

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

        uint64_t word;
        std::memcpy(&word, state.buffer + 0, 8);
        state.acc[0] = mix(state.acc[0], word);
        std::memcpy(&word, state.buffer + 8, 8);
        state.acc[1] = mix(state.acc[1], word);
        std::memcpy(&word, state.buffer + 16, 8);
        state.acc[2] = mix(state.acc[2], word);
        std::memcpy(&word, state.buffer + 24, 8);
        state.acc[3] = mix(state.acc[3], word);

        state.buf_used = 0; // buffer is consumed
        data += remaining; // advance past the bytes you already used
        len -= remaining;
    }

    for (size_t i = 0; i + 32 <= len; i += 32)
    {

        uint64_t word;

        std::memcpy(&word, data + i + 0, 8);
        state.acc[0] = mix(state.acc[0], word);

        std::memcpy(&word, data + i + 8, 8);
        state.acc[1] = mix(state.acc[1], word);

        std::memcpy(&word, data + i + 16, 8);
        state.acc[2] = mix(state.acc[2], word);

        std::memcpy(&word, data + i + 24, 8);
        state.acc[3] = mix(state.acc[3], word);
    }

    size_t remaining = len % 32;
    std::memcpy(state.buffer, data + (len - remaining), remaining);
    state.buf_used = remaining;
}

export auto phanes_hash_digest(PhanesHashState& state) -> uint64_t
{
    uint64_t h64 = rotate_left(state.acc[0], 1) + rotate_left(state.acc[1], 7) + rotate_left(state.acc[2], 12) +
        rotate_left(state.acc[3], 18);

    h64 += state.total_len;

    for (size_t i = 0; i < state.buf_used; i++)
    {
        h64 ^= (uint64_t)state.buffer[i] << ((i % 8) * 8);
    }

    h64 ^= h64 >> 33;
    h64 *= PRIME_3;
    h64 ^= h64 >> 29;

    return h64;
}