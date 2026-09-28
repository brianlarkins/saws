#pragma once

#include "config.hpp"
#include "run_control.hpp"
#include "stats.hpp"
#include "saws_ring.hpp"

namespace gputc {

template<class Task, class Cfg>
struct KernelArgs {
  using Body  = typename Task::Body;
  using State = RingState<Cfg::epochs>;

  RunControl*         run;
  unsigned long long* spawned;
  unsigned long long* executed;
  State*              states;
  Body*               bodies;
  WarpCounters*       out;
  int                 worker_count;
  Policy              policy;
};

} // namespace gputc
