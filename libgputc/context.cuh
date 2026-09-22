#pragma once

#include "warp.cuh"

namespace gputc {

// The device API a task sees. Task::execute runs on all 32 lanes of a warp,
// one task per lane; lanes past the end of a partial chunk get valid = false.
// spawn and warp_max are warp-collective: every lane must reach each call,
// passing pred = false if it has nothing to spawn.
template<class Worker>
struct Ctx {
  using Body = typename Worker::Body;

  Worker& worker;

  __device__ int lane() const { return gputc::warp::lane(); }
  __device__ int warp() const { return gputc::warp::id(); }
  __device__ int warp_max(int v) const { return gputc::warp::reduce_max(v); }

  __device__ void spawn(const Body& body, bool pred = true) { worker.spawn(body, pred); }
};

} // namespace gputc
