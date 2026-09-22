#pragma once

#include "common.hpp"

namespace gputc {

// Compile-time layout. Derive from DefaultConfig and override to change it.
struct DefaultConfig {
  static constexpr int  warps         = 8;     // warps in the persistent block
  static constexpr int  chunk         = 32;    // tasks per steal unit (lane width)
  static constexpr int  warp_chunks   = 8;     // warp deque capacity, chunks
  static constexpr int  warp_epochs   = 8;     // epoch slots per warp deque
  static constexpr int  global_chunks = 4096;  // global ring capacity, chunks
  static constexpr int  global_epochs = 16;    // epoch slots in the global ring
  static constexpr bool timing        = true;  // per-phase cycle accounting in Stats
};

// Runtime knobs, passed to the kernel by value.
struct Policy {
  int release_min_chunks = 2;        // private full chunks before a warp publishes
  int probe_retries      = 4;        // sibling claims attempted per search
  int backoff_min_ns     = 64;       // idle backoff bounds
  int backoff_max_ns     = 8192;
  int pinned_wait_ns     = 64;       // deque full of chunks thieves are still copying
  int lock_backoff_ns    = 64;       // global ring lock
};

} // namespace gputc
