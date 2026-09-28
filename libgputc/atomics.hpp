#pragma once

#include <cuda/atomic>

#include "common.hpp"

namespace gputc {

struct DeviceScope {
  template<class T> using ref = cuda::atomic_ref<T, cuda::thread_scope_device>;

  template<class T> static GPUTC_HD T load_relaxed(T* p) {
    return ref<T>(*p).load(cuda::memory_order_relaxed);
  }
  template<class T> static GPUTC_HD T load_acquire(T* p) {
    return ref<T>(*p).load(cuda::memory_order_acquire);
  }
  template<class T> static GPUTC_HD void store_relaxed(T* p, T v) {
    ref<T>(*p).store(v, cuda::memory_order_relaxed);
  }
  template<class T> static GPUTC_HD void store_release(T* p, T v) {
    ref<T>(*p).store(v, cuda::memory_order_release);
  }
  template<class T> static GPUTC_HD T fetch_add_acq_rel(T* p, T v) {
    return ref<T>(*p).fetch_add(v, cuda::memory_order_acq_rel);
  }
  template<class T> static GPUTC_HD T fetch_add_release(T* p, T v) {
    return ref<T>(*p).fetch_add(v, cuda::memory_order_release);
  }
  template<class T> static GPUTC_HD T fetch_or_acq_rel(T* p, T v) {
    return ref<T>(*p).fetch_or(v, cuda::memory_order_acq_rel);
  }
};

} // namespace gputc
