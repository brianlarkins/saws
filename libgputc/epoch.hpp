#pragma once

// Closed epochs awaiting reclamation, in ring order.
//
// Sequence numbers [head, open) are closed epochs whose chunks may still be in
// transit to thieves. Sequence `open` names the slot of the epoch currently
// published in the steal word (if any). Slot of a sequence is seq % N, which is
// also the epoch field thieves read from the steal word.

#include "common.hpp"
#include "StealWord.hpp"

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
  uint32_t open;
  EpochRec rec[N];

  static GPUTC_HD constexpr uint32_t slot(uint32_t seq) { return seq & (N - 1); }
  GPUTC_HD uint32_t pending() const { return open - head; }
  GPUTC_HD bool empty() const { return head == open; }

  // Closing the open epoch may push one more entry; the new open epoch then
  // needs its own slot, so at most N - 1 entries may be pending afterwards.
  GPUTC_HD bool can_close_and_reopen() const { return pending() + 2 <= uint32_t(N); }

  GPUTC_HD EpochRec& front() { return rec[slot(head)]; }
  GPUTC_HD EpochRec& current() { return rec[slot(open)]; }
};

} // namespace gputc
