#pragma once

// Closed epochs awaiting reclamation, in ring order.
//
// Sequence numbers [head, next) are closed epochs whose chunks may still be in
// transit to thieves. The epoch published in the steal word (if any) uses
// sequence `next`. The slot of a sequence is seq % N, which is also the epoch
// field thieves read from the steal word.

#include "common.hpp"
#include "steal_word.hpp"

namespace gputc {

struct EpochRec {
  uint32_t retired;  // chunks copied out by thieves (thieves add, owner resets)
  uint32_t taken;    // chunks claimed by valid tickets, fixed at close
  uint32_t end;      // task position one past the epoch's taken range
};

template<int N>
struct EpochFifo {
  static_assert(N >= 2 && N <= int(MAX_EPOCHS) && is_pow2(N), "epoch count must fit the steal word");

  uint32_t head;
  uint32_t next;
  EpochRec rec[N];

  static GPUTC_HD constexpr uint32_t slot(uint32_t seq) { return seq & (N - 1); }
  GPUTC_HD uint32_t pending() const { return next - head; }
  GPUTC_HD bool empty() const { return head == next; }

  // Closing the published epoch may add one pending entry; the epoch published
  // after it then needs its own slot, so at most N - 1 may be pending.
  GPUTC_HD bool can_close_and_publish() const { return pending() + 2 <= uint32_t(N); }

  GPUTC_HD EpochRec& oldest() { return rec[slot(head)]; }
  GPUTC_HD EpochRec& published() { return rec[slot(next)]; }
};

} // namespace gputc
