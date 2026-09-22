#pragma once

#include <cstdint>

#if defined(__CUDACC__)
#define GPUTC_HD __host__ __device__ __forceinline__
#else
#define GPUTC_HD inline
#endif

namespace gputc {

GPUTC_HD constexpr bool is_pow2(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

GPUTC_HD constexpr uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

} // namespace gputc
