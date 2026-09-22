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

// Always-on invariant check: traps on the device, aborts on the host.
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

} // namespace gputc
