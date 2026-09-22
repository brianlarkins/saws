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

// SM cycle counter. A non-template wrapper, so templates that call it still
// parse in nvcc's host pass.
__device__ __forceinline__ long long clock() { return clock64(); }

// Sum of v over this lane and the lanes below it.
__device__ __forceinline__ uint32_t inclusive_sum(uint32_t v) {
#pragma unroll
  for (int d = 1; d < SIZE; d <<= 1) {
    uint32_t below = __shfl_up_sync(FULL, v, d);
    if (lane() >= d) v += below;
  }
  return v;
}

// The lane owning item g when lane l owns items [inclusive_l - count_l,
// inclusive_l): the number of lanes whose inclusive sum is at most g.
// Requires g < the warp's total; lanes may ask about different g.
__device__ __forceinline__ int owner_of(uint32_t inclusive, uint32_t g) {
  int lo = 0;
#pragma unroll
  for (int step = SIZE / 2; step > 0; step >>= 1)
    if (__shfl_sync(FULL, inclusive, lo + step - 1) <= g) lo += step;
  return lo;
}

// Read any trivially copyable value from lane src (which may differ per lane).
template<class T>
__device__ __forceinline__ T shfl(const T& v, int src) {
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

template<class T>
__device__ __forceinline__ T bcast(const T& v) { return shfl(v, 0); }

// Run f on lane 0 and broadcast its result. The __syncwarp orders lane 0's
// memory operations in f before every lane's accesses after the call, so the
// warp may touch slots that f claimed, reserved or reclaimed.
template<class F>
__device__ __forceinline__ auto lead(F&& f) {
  decltype(f()) r{};
  if (leader()) r = f();
  __syncwarp();
  return bcast(r);
}

// A uniformly random set bit of a nonzero mask.
__device__ __forceinline__ int pick_random(unsigned mask, uint32_t r) {
  for (uint32_t k = r % __popc(mask); k > 0; --k) mask &= mask - 1;
  return __ffs(mask) - 1;
}

// ---- moving task bodies ----
//
// Bodies whose size is a multiple of 16 bytes move as 16-byte words. Ring
// storage starts 16-byte aligned (the shared-memory carve-up and cudaMalloc),
// so every slot of such a body is aligned too.

template<class T>
__device__ __forceinline__ T load_body(const T* p) {
  if constexpr (sizeof(T) % 16 == 0) {
    uint4 w[sizeof(T) / 16];
#pragma unroll
    for (int i = 0; i < int(sizeof(T) / 16); ++i) w[i] = reinterpret_cast<const uint4*>(p)[i];
    T r;
    __builtin_memcpy(&r, w, sizeof(T));
    return r;
  } else {
    return *p;
  }
}

template<class T>
__device__ __forceinline__ void store_body(T* p, const T& v) {
  if constexpr (sizeof(T) % 16 == 0) {
    uint4 w[sizeof(T) / 16];
    __builtin_memcpy(w, &v, sizeof(T));
#pragma unroll
    for (int i = 0; i < int(sizeof(T) / 16); ++i) reinterpret_cast<uint4*>(p)[i] = w[i];
  } else {
    *p = v;
  }
}

// Copy N contiguous bodies with the whole warp. Lanes take consecutive words,
// so global accesses coalesce and shared accesses do not bank-conflict.
template<int N, class T>
__device__ __forceinline__ void copy_bodies(T* dst, const T* src) {
  if constexpr ((N * sizeof(T)) % (16 * SIZE) == 0) {
    constexpr int words = int(N * sizeof(T) / 16);
    const uint4* s = reinterpret_cast<const uint4*>(src);
    uint4* d = reinterpret_cast<uint4*>(dst);
#pragma unroll
    for (int i = 0; i < words; i += SIZE) d[i + lane()] = s[i + lane()];
  } else {
    for (int i = lane(); i < N; i += SIZE) dst[i] = src[i];
  }
}

} // namespace warp
} // namespace gputc
