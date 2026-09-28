#pragma once

// Device-wide termination. Worker 0 lane 0 sums every executed counter, then
// every spawned counter (two passes). Equal totals with no error flag end the run.

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
    unsigned long long v = DeviceScope::load_relaxed(&spawned[w]);
    DeviceScope::store_release(&spawned[w], v + n);
  }

  __device__ void add_executed(int w, unsigned n) {
    unsigned long long v = DeviceScope::load_relaxed(&executed[w]);
    DeviceScope::store_release(&executed[w], v + n);
  }

  __device__ bool aborted() const {
    return DeviceScope::load_acquire(&run->error_code) != ERR_NONE;
  }

  __device__ bool done() const {
    return DeviceScope::load_acquire(&run->done) != 0;
  }

  __device__ void record_error(int worker, ErrorCode code, uint32_t want = 0, uint32_t avail = 0) {
    int expected = ERR_NONE;
    if (!DeviceScope::cas_weak_acq_rel(&run->error_code, expected, int(code))) return;
    run->error_worker = worker;
    run->error_want = want;
    run->error_avail = avail;
  }

  __device__ bool try_finish() {
    if (done() || aborted()) return done();
    unsigned long long e = 0, s = 0;
    for (int i = 0; i < worker_count; ++i) {
      unsigned long long v = DeviceScope::load_acquire(&executed[i]);
      if (u64_would_overflow(e, v)) {
        record_error(0, ERR_COUNTER_OVERFLOW);
        return false;
      }
      e += v;
    }
    for (int i = 0; i < worker_count; ++i) {
      unsigned long long v = DeviceScope::load_acquire(&spawned[i]);
      if (u64_would_overflow(s, v)) {
        record_error(0, ERR_COUNTER_OVERFLOW);
        return false;
      }
      s += v;
    }
    if (e != s) return false;
    DeviceScope::store_release(&run->done, 1u);
    return true;
  }
};

} // namespace gputc
