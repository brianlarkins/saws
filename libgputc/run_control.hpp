#pragma once

#include "common.hpp"

namespace gputc {

enum ErrorCode : int {
  ERR_NONE = 0,
  ERR_QUEUE_CAPACITY = 1,
  ERR_STEAL_COUNTER_GUARD = 2,
  ERR_COUNTER_OVERFLOW = 3,
};

// Device-visible run state. The first failure is recorded; all workers poll
// `error_code` and eventually exit. `done` is set only on successful termination.
struct RunControl {
  uint32_t done;
  int      error_code;
  int      error_worker;
  uint32_t error_want;
  uint32_t error_avail;
};

constexpr uint32_t STEAL_ATTEMPT_GUARD = 1u << 31;

// With one outstanding claim per worker, worker_count must stay below 2^31.
GPUTC_HD constexpr bool valid_worker_count(int n) {
  return n > 0 && uint32_t(n) < STEAL_ATTEMPT_GUARD;
}

} // namespace gputc
