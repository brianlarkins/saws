#pragma once

#include <cuda_runtime.h>
#include <cstdint>
#include <type_traits>

namespace saws::detail::warp {

inline constexpr unsigned width = 32;
inline constexpr unsigned mask = 0xffffffffu;

__device__ inline unsigned lane() { return threadIdx.x & (width - 1); }
__device__ inline void join() { __syncwarp(mask); }

// Register exchange only. Call join() separately when transferring visibility
// of memory operations. The source lane may differ for each receiving lane.
template<class T>
__device__ inline T broadcast(T value, unsigned source = 0) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto* bytes = reinterpret_cast<unsigned char*>(&value);
    for (unsigned base = 0; base < sizeof(T); base += sizeof(unsigned)) {
        unsigned word = 0;
        for (unsigned j = 0; j < sizeof(unsigned) && base + j < sizeof(T); ++j)
            word |= unsigned(bytes[base + j]) << (8 * j);
        word = __shfl_sync(mask, word, source);
        for (unsigned j = 0; j < sizeof(unsigned) && base + j < sizeof(T); ++j)
            bytes[base + j] = static_cast<unsigned char>(word >> (8 * j));
    }
    return value;
}

__device__ inline std::uint64_t inclusive_sum(std::uint64_t value) {
    for (unsigned offset = 1; offset < width; offset <<= 1) {
        const auto other = broadcast(value, lane() >= offset ? lane() - offset : 0);
        if (lane() >= offset) value += other;
    }
    return value;
}

template<class T>
__device__ inline T maximum(T value) {
    for (unsigned offset = width / 2; offset; offset >>= 1) {
        const T other = broadcast(value, lane() ^ offset);
        if (other > value) value = other;
    }
    return value;
}

__device__ inline void backoff() {
#if __CUDA_ARCH__ >= 700
    __nanosleep(128);
#endif
}

} // namespace saws::detail::warp
