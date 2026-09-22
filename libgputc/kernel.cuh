#pragma once

// The persistent kernel. It is a template so it is instantiated in the
// application's translation unit, where Task::execute can be inlined.

#include <type_traits>

#include "scheduler.cuh"

namespace gputc {

template<class Task, class Cfg>
constexpr size_t deque_shared_bytes() {
  return size_t(Cfg::warps) * Cfg::warp_chunks * Cfg::chunk * sizeof(typename Task::Body);
}

template<class Task, class Cfg>
__global__ void __launch_bounds__(Cfg::warps * warp::SIZE, 1)
persistent_kernel(KernelArgs<Task, Cfg> args) {
  using Body = typename Task::Body;
  static_assert(std::is_trivially_copyable<Body>::value, "task bodies are moved as raw memory");
  static_assert(alignof(Body) <= 16, "task body alignment exceeds the shared-memory carve-up");
  static_assert(Cfg::chunk == warp::SIZE, "lane-width tasks: one chunk is one task per lane");
  static_assert(Cfg::warps >= 1 && Cfg::warps <= warp::SIZE, "a probe covers at most 32 sibling warps");

  extern __shared__ __align__(16) unsigned char gputc_dynamic_smem[];
  __shared__ Shared<Cfg> sh;
  Body* deques = reinterpret_cast<Body*>(gputc_dynamic_smem);

  const int me = warp::id();
  if (warp::leader()) {
    sh.warp_state[me].init();
    sh.term.init(me);
    sh.counters[me] = WarpCounters{};
  }
  if (threadIdx.x == 0) sh.term.done = 0;
  __syncthreads();

  Worker<Task, Cfg> worker(sh, deques, args);
  if (me == 0) worker.seed(args.root);
  __syncthreads();

  worker.run();
  worker.finish(args.out);
}

} // namespace gputc
