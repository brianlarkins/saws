#pragma once

#include <cstdint>

namespace gputc {
namespace warp {

constexpr unsigned FULL = 0xffffffffu;
constexpr int SIZE = 32;

__device__ __forceinline__ int lane() { return threadIdx.x & (SIZE - 1); }
__device__ __forceinline__ int id() { return threadIdx.x / SIZE; }
__device__ __forceinline__ bool leader() { return lane() == 0; }
__device__ __forceinline__ unsigned lanemask_lt() { return (1u << lane()) - 1u; }

__device__ __forceinline__ int reduce_max(int v) { return __reduce_max_sync(FULL, v); }

// Broadcast any trivially copyable value from one lane to the whole warp.
template<class T>
__device__ __forceinline__ T bcast(const T& v, int src = 0) {
  constexpr int words = (sizeof(T) + 3) / 4;
  uint32_t in[words] = {};
  uint32_t out[words];
  __builtin_memcpy(in, &v, sizeof(T));
#pragma unroll
  for (int i = 0; i < words; ++i) out[i] = __shfl_sync(FULL, in[i], src);
  T r;
  __builtin_memcpy(&r, out, sizeof(T));
  return r;
}

// Lane index of a set bit of mask, starting the search at lane `start`.
__device__ __forceinline__ int pick(unsigned mask, int start) {
  unsigned rotated = __funnelshift_r(mask, mask, start);
  return (__ffs(rotated) - 1 + start) & (SIZE - 1);
}

} // namespace warp
} // namespace gputc
