#pragma once

// Single-block termination. Each warp's lane 0 is the only writer of its own
// counters. A checker sums every `executed` before any `spawned`: a task's
// spawn happens before its execution, which happens before its executed
// increment, so the executed set it sees is a subset of the spawned set it
// reads afterwards. Equal sums therefore mean nothing is pending anywhere.

#include "atomics.hpp"

namespace gputc {

template<int Warps>
struct Termination {
  unsigned long long spawned[Warps];
  unsigned long long executed[Warps];
  int done;

  __device__ void init(int w) {
    spawned[w] = 0;
    executed[w] = 0;
  }

  __device__ void add_spawned(int w, unsigned n) {
    BlockScope::store_release(&spawned[w], BlockScope::load_relaxed(&spawned[w]) + n);
  }

  __device__ void add_executed(int w, unsigned n) {
    BlockScope::store_release(&executed[w], BlockScope::load_relaxed(&executed[w]) + n);
  }

  __device__ bool finished() {
    if (BlockScope::load_acquire(&done)) return true;
    unsigned long long e = 0, s = 0;
    for (int i = 0; i < Warps; ++i) e += BlockScope::load_acquire(&executed[i]);
    for (int i = 0; i < Warps; ++i) s += BlockScope::load_acquire(&spawned[i]);
    if (s != e) return false;
    BlockScope::store_release(&done, 1);
    return true;
  }
};

} // namespace gputc
