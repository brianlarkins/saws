#pragma once

#include <type_traits>

#include "kernel_args.cuh"
#include "scheduler.cuh"

namespace gputc {

template<class Task, class Cfg>
__global__ void init_queues(KernelArgs<Task, Cfg> args, typename Task::Body root) {
  const int w = blockIdx.x * (blockDim.x / warp::SIZE) + warp::id();
  if (w >= args.worker_count) return;
  if (warp::leader()) {
    args.states[w].init(0);
    args.spawned[w] = 0;
    args.executed[w] = 0;
  }
  if (w == 0 && warp::leader()) {
    args.bodies[0] = root;
    args.states[0].head = 1;
    args.spawned[0] = 1;
  }
}

template<class Task, class Cfg>
__global__ void __launch_bounds__(Cfg::warps_per_block * warp::SIZE)
persistent_kernel(KernelArgs<Task, Cfg> args) {
  using Body = typename Task::Body;
  static_assert(std::is_trivially_copyable<Body>::value, "task bodies are trivially copyable");
  static_assert(alignof(Body) <= 16, "task body alignment exceeds 16 bytes");
  static_assert(Cfg::chunk == warp::SIZE, "one chunk is one task per lane");
  static_assert(Cfg::warps_per_block >= 1, "at least one warp per block");

  const int w = blockIdx.x * (blockDim.x / warp::SIZE) + warp::id();
  if (w >= args.worker_count) return;

  Worker<Task, Cfg> worker(args, w);
  worker.run();
  worker.finish();
}

} // namespace gputc
