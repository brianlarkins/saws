#pragma once

// Atomic scope policies. SawsRing is written once against this interface and
// instantiated with BlockScope (shared-memory warp deques), DeviceScope (the
// global ring), or HostScope (host tests under std::thread).

#include "common.hpp"

#if defined(__CUDACC__)
#include <cuda/atomic>
#endif

namespace gputc {

#if defined(__CUDACC__)

template<cuda::thread_scope S>
struct CudaScope {
  template<class T> using ref = cuda::atomic_ref<T, S>;

  template<class T> static GPUTC_HD T load_relaxed(T* p) { return ref<T>(*p).load(cuda::memory_order_relaxed); }
  template<class T> static GPUTC_HD T load_acquire(T* p) { return ref<T>(*p).load(cuda::memory_order_acquire); }
  template<class T> static GPUTC_HD void store_relaxed(T* p, T v) { ref<T>(*p).store(v, cuda::memory_order_relaxed); }
  template<class T> static GPUTC_HD void store_release(T* p, T v) { ref<T>(*p).store(v, cuda::memory_order_release); }
  template<class T> static GPUTC_HD T fetch_add_acq_rel(T* p, T v) { return ref<T>(*p).fetch_add(v, cuda::memory_order_acq_rel); }
  template<class T> static GPUTC_HD T fetch_add_release(T* p, T v) { return ref<T>(*p).fetch_add(v, cuda::memory_order_release); }
  template<class T> static GPUTC_HD T fetch_or_acq_rel(T* p, T v) { return ref<T>(*p).fetch_or(v, cuda::memory_order_acq_rel); }
  template<class T> static GPUTC_HD bool cas_acquire(T* p, T expected, T desired) {
    return ref<T>(*p).compare_exchange_strong(expected, desired, cuda::memory_order_acquire, cuda::memory_order_relaxed);
  }
};

using BlockScope  = CudaScope<cuda::thread_scope_block>;
using DeviceScope = CudaScope<cuda::thread_scope_device>;

#else

struct HostScope {
  template<class T> static T load_relaxed(T* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
  template<class T> static T load_acquire(T* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
  template<class T> static void store_relaxed(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
  template<class T> static void store_release(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
  template<class T> static T fetch_add_acq_rel(T* p, T v) { return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL); }
  template<class T> static T fetch_add_release(T* p, T v) { return __atomic_fetch_add(p, v, __ATOMIC_RELEASE); }
  template<class T> static T fetch_or_acq_rel(T* p, T v) { return __atomic_fetch_or(p, v, __ATOMIC_ACQ_REL); }
  template<class T> static bool cas_acquire(T* p, T expected, T desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  }
};

#endif

} // namespace gputc
