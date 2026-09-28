#pragma once

#include "common.hpp"

namespace gputc {

// Compile-time layout. Derive from DefaultConfig and override fields or pass
// QUEUE_CHUNKS= on the Makefile command line.
struct DefaultConfig {
  static constexpr int  warps_per_block = 8;
  static constexpr int  chunk           = 32;
  static constexpr int  deque_chunks    = 256;
  static constexpr int  epochs          = 8;
  static constexpr bool timing          = false;
};

constexpr int TERMINATION_CHECK_INTERVAL = 64;

struct LaunchOptions {
  int blocks = 0;  // 0 = occupancy-based automatic grid size
};

struct Policy {
  int backoff_min_ns = 64;
  int backoff_max_ns = 8192;
  int pinned_wait_ns = 64;
};

} // namespace gputc
