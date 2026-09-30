#pragma once

#include <cuda_runtime.h>
#include <cstdint>

namespace uts::rng {

// SHA-1 digest words in big-endian numeric order. The root and child messages
// match the byte encoding in examples/uts/rng/brg_sha1.c.
struct State { std::uint32_t word[5]; };

__host__ __device__ inline std::uint32_t rotate(std::uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

// UTS uses only 20- and 24-byte messages, so one padded SHA-1 block suffices.
__host__ __device__ inline State digest(std::uint32_t (&w)[16]) {
    std::uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu;
    std::uint32_t d = 0x10325476u, e = 0xc3d2e1f0u;
    for (unsigned t = 0; t < 80; ++t) {
        if (t >= 16)
            w[t & 15] = rotate(w[(t - 3) & 15] ^ w[(t - 8) & 15] ^
                               w[(t - 14) & 15] ^ w[t & 15], 1);
        std::uint32_t f, k;
        if (t < 20) {
            f = (b & c) | (~b & d); k = 0x5a827999u;
        } else if (t < 40) {
            f = b ^ c ^ d; k = 0x6ed9eba1u;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d; k = 0xca62c1d6u;
        }
        const auto next = rotate(a, 5) + f + e + k + w[t & 15];
        e = d; d = c; c = rotate(b, 30); b = a; a = next;
    }
    return {{0x67452301u + a, 0xefcdab89u + b, 0x98badcfeu + c,
             0x10325476u + d, 0xc3d2e1f0u + e}};
}

__host__ __device__ inline State root(unsigned seed) {
    std::uint32_t message[16]{};
    message[4] = seed;
    message[5] = 0x80000000u;
    message[15] = 20 * 8;
    return digest(message);
}

__host__ __device__ inline State child(const State& parent, unsigned index) {
    std::uint32_t message[16]{};
    for (unsigned i = 0; i < 5; ++i) message[i] = parent.word[i];
    message[5] = index;
    message[6] = 0x80000000u;
    message[15] = 24 * 8;
    return digest(message);
}

__host__ __device__ inline double probability(const State& state) {
    return double(state.word[4] & 0x7fffffffu) / 2147483648.0;
}

} // namespace uts::rng
