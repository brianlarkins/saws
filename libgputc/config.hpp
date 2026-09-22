#pragma once

#include "common.hpp"

namespace gputc {

// Compile-time layout. Derive from DefaultConfig and override to change it.
struct DefaultConfig {
  static constexpr int warps         = 8;     // warps in the persistent block
  static constexpr int chunk         = 32;    // tasks per steal unit (lane width)
  static constexpr int epochs        = 16;    // epoch slots per ring
  static constexpr int warp_chunks   = 8;     // warp deque capacity, chunks
  static constexpr int global_chunks = 4096;  // global ring capacity, chunks
};

// Runtime knobs, passed to the kernel by value.
struct Policy {
  int release_min_chunks = 2;     // private full chunks before a warp publishes
  int probe_retries      = 4;     // sibling claims attempted per search
  int backoff_min_ns     = 64;
  int backoff_max_ns     = 8192;
};

} // namespace gputc
