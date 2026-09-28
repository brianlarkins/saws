#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__CUDACC__)
#define GPUTC_HD __host__ __device__ __forceinline__
#define GPUTC_UNROLL _Pragma("unroll")
#else
#define GPUTC_HD inline
#define GPUTC_UNROLL
#endif

#if defined(__CUDA_ARCH__)
#define GPUTC_CHECK(cond, msg)                                                   \
  do {                                                                           \
    if (!(cond)) {                                                               \
      printf("gputc: %s:%d: %s (%s)\n", __FILE__, __LINE__, msg, #cond);         \
      __trap();                                                                  \
    }                                                                            \
  } while (0)
#else
#define GPUTC_CHECK(cond, msg)                                                   \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "gputc: %s:%d: %s (%s)\n", __FILE__, __LINE__, msg, #cond); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)
#endif

namespace gputc {

GPUTC_HD constexpr bool is_pow2(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

GPUTC_HD constexpr uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
GPUTC_HD constexpr uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

GPUTC_HD constexpr uint32_t bit_width(uint32_t x) {
#if defined(__CUDA_ARCH__)
  return x == 0 ? 0u : 32u - __clz(x);
#else
  if (x == 0) return 0;
  return 32u - __builtin_clz(x);
#endif
}

GPUTC_HD constexpr uint32_t ceil_div_u32(uint32_t a, uint32_t b) { return b ? (a + b - 1) / b : 0; }

GPUTC_HD constexpr bool u64_would_overflow(unsigned long long a, unsigned long long b) {
  return a > ~0ull - b;
}

} // namespace gputc
