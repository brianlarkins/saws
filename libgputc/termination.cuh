#pragma once

// Device-wide termination. Worker 0 sums every executed counter, then every
// spawned counter (two passes). Equal totals with no error flag mean the rooted
// computation has no pending frontier.

#include "atomics.hpp"
#include "config.hpp"
#include "run_control.hpp"

namespace gputc {

struct TerminationView {
  unsigned long long* spawned;
  unsigned long long* executed;
  RunControl*         run;
  int                 worker_count;

  __device__ void add_spawned(int w, unsigned n) {
    DeviceScope::store_release(&spawned[w], DeviceScope::load_relaxed(&spawned[w]) + n);
  }

  __device__ void add_executed(int w, unsigned n) {
    DeviceScope::store_release(&executed[w], DeviceScope::load_relaxed(&executed[w]) + n);
  }

  __device__ bool aborted() const {
    return DeviceScope::load_acquire(&run->error_code) != ERR_NONE;
  }

  __device__ bool done() const {
    return DeviceScope::load_acquire(&run->done) != 0;
  }

  __device__ void record_error(int worker, ErrorCode code, uint32_t want = 0, uint32_t avail = 0) {
    if (DeviceScope::load_relaxed(&run->error_code) != ERR_NONE) return;
    run->error_worker = worker;
    run->error_want = want;
    run->error_avail = avail;
    DeviceScope::store_release(&run->error_code, int(code));
  }

  __device__ bool try_finish() {
    if (done() || aborted()) return done();
    unsigned long long e = 0, s = 0;
    for (int i = 0; i < worker_count; ++i)
      e += DeviceScope::load_acquire(&executed[i]);
    for (int i = 0; i < worker_count; ++i)
      s += DeviceScope::load_acquire(&spawned[i]);
    if (e != s) return false;
    DeviceScope::store_release(&run->done, 1u);
    return true;
  }

  __device__ bool check_due(int worker, int iter) {
    if (worker != 0) return false;
    if ((iter & (TERMINATION_CHECK_INTERVAL - 1)) != 0) return false;
    return try_finish();
  }
};

} // namespace gputc
