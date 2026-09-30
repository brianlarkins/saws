#pragma once

#include <cstdint>

namespace saws::detail {

inline constexpr unsigned chunk_size = 32;
inline constexpr unsigned epoch_slots = 8;
inline constexpr unsigned max_queue_capacity = 4096 * chunk_size;
inline constexpr std::uint32_t attempt_limit = std::uint32_t{1} << 31;
inline constexpr std::uint64_t closed_bit = std::uint64_t{1} << 31;
inline constexpr std::uint64_t attempt_increment = std::uint64_t{1} << 32;

// Pure arithmetic; these functions also work in a regular C++ translation unit.
#ifdef __CUDACC__
#define SAWS_HD __host__ __device__
#else
#define SAWS_HD
#endif

SAWS_HD constexpr unsigned bit_width(unsigned value) {
    unsigned width = 0;
    while (value) {
        ++width;
        value >>= 1;
    }
    return width;
}

SAWS_HD constexpr unsigned remaining(unsigned chunks, unsigned ticket) {
    if (chunks == 0 || ticket > bit_width(chunks - 1)) return 0;
    return 1 + ((chunks - 1) >> ticket);
}

struct Share {
    unsigned first;
    unsigned count;
};

SAWS_HD constexpr Share half_share(unsigned chunks, unsigned ticket) {
    const unsigned before = remaining(chunks, ticket);
    // Guard exhausted tickets before incrementing an arbitrary ticket value.
    if (before == 0) return {chunks, 0};
    return {chunks - before, before - remaining(chunks, ticket + 1)};
}

// One atomic word: attempts[63:32], closed[31], slot[30:27],
// initial chunks[26:14], and physical base chunk[13:0].
struct Descriptor {
    std::uint64_t bits;

    SAWS_HD constexpr unsigned attempts() const { return unsigned(bits >> 32); }
    SAWS_HD constexpr bool closed() const { return (bits & closed_bit) != 0; }
    SAWS_HD constexpr unsigned slot() const { return unsigned(bits >> 27) & 15; }
    SAWS_HD constexpr unsigned chunks() const { return unsigned(bits >> 14) & 8191; }
    SAWS_HD constexpr unsigned base() const { return unsigned(bits) & 16383; }

    SAWS_HD static constexpr Descriptor open(unsigned slot, unsigned chunks,
                                              unsigned base) {
        return {(std::uint64_t(slot) << 27) |
                (std::uint64_t(chunks) << 14) | base};
    }
};

#undef SAWS_HD

} // namespace saws::detail
