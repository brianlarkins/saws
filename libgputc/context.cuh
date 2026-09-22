#pragma once

#include "warp.cuh"

namespace gputc {

// The device API a task sees. Task::execute runs on all 32 lanes of a warp,
// one task per lane; lanes past the end of a partial chunk get valid = false.
// spawn, spawn_n and warp_max are warp-collective: every lane must reach each
// call, passing pred = false or count = 0 if it has nothing to spawn.
template<class Worker>
struct Ctx {
  using Body = typename Worker::Body;

  Worker& worker;

  __device__ int lane() const { return gputc::warp::lane(); }
  __device__ int warp() const { return gputc::warp::id(); }
  __device__ int warp_max(int v) const { return gputc::warp::reduce_max(v); }

  // Spawn one task per lane where pred holds.
  __device__ void spawn(const Body& body, bool pred = true) { worker.spawn(body, pred); }

  // Spawn `count` tasks from this lane: make(parent, i) builds the i-th, for
  // i in [0, count). Lanes may pass different counts. The warp spreads the
  // children over its lanes, so each round builds 32 of them however unevenly
  // they are split between parents; `parent` is copied to the lane that builds
  // each child, so it must be trivially copyable. make must not use warp
  // collectives.
  template<class Parent, class Make>
  __device__ void spawn_n(int count, const Parent& parent, Make&& make) {
    worker.spawn_n(count > 0 ? uint32_t(count) : 0u, parent, make);
  }
};

} // namespace gputc
