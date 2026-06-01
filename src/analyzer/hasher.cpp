module;

#include <cstdint>
#include <cstring>
#include <immintrin.h> 

module analyzer:hasher;

static constexpr uint64_t PRIME_1 = 0x9E3779B185EBCA87ULL;
static constexpr uint64_t PRIME_2 = 0xC2B2AE3D27D4EB4FULL;
static constexpr uint64_t PRIME_3 = 0x165667B19E3779F9ULL;


struct PhanesHashState {
    uint64_t acc[4];
    uint8_t  buffer[32];
    size_t   buf_used;
    size_t   total_len;
};


static inline auto rotate_left(uint64_t x, int n) -> uint64_t
{
    return (x << n) | (x >> (64 - n));
}

auto phanes_hash(const uint8_t* data, size_t len) -> uint64_t
{
    // initialize acc with non zero
    uint64_t acc = 0;
    for (size_t i = 0; i + 8 <= len; i += 8)
    {
        uint64_t word;

        std::memcpy(&word, &data[i], 8);
        acc ^= word * PRIME_1;
        acc = rotate_left(acc, 31);
        acc *= PRIME_2;
    }

    acc ^= acc >> 33;
    acc *= PRIME_3;
    acc ^= acc >> 29;

    // handle remaining < 8 bytes
    size_t tail = len & ~(size_t)7; // = (len / 8) * 8
    for (size_t i = tail; i < len; i++)
    {
        acc ^= (uint64_t)data[i] << ((i % 8) * 8);
    }

    return acc;
}
