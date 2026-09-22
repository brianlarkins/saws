#pragma once

namespace gputc {

// Per-warp event counts, summed over the block at exit.
struct Counters {
  unsigned long long executed;
  unsigned long long spawned;
  unsigned long long steals_sibling;
  unsigned long long steals_global;
  unsigned long long steal_misses;   // probe said stealable, ticket was invalid
  unsigned long long releases;
  unsigned long long reacquires;
  unsigned long long promotes;       // warp deque overflows moved to the global ring
};

struct Stats : Counters {
  double seconds;
};

} // namespace gputc
