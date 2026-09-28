#pragma once

// The 64-bit steal word. All counts are in chunks.
//
//   63            32   31    30   27  26      14  13      0
//  +----------------+-------+-------+-----------+----------+
//  |    attempts    | closed| epoch |  initial  |   base   |
//  +----------------+-------+-------+-----------+----------+
//
// attempts is the high half: thieves fetch_add (1 << 32) to issue one ticket.
// initial is the published chunk count for this epoch. Each ticket receives a
// geometric half share via half_claim(initial, ticket_index).

#include "common.hpp"
#include "run_control.hpp"

namespace gputc {

constexpr int BITS_BASE     = 14;
constexpr int BITS_INITIAL  = 13;
constexpr int BITS_EPOCH    = 4;

constexpr int SHIFT_BASE    = 0;
constexpr int SHIFT_INITIAL = SHIFT_BASE + BITS_BASE;
constexpr int SHIFT_EPOCH   = SHIFT_INITIAL + BITS_INITIAL;
constexpr int SHIFT_CLOSED  = SHIFT_EPOCH + BITS_EPOCH;
constexpr int ATTEMPT_SHIFT = 32;

static_assert(SHIFT_CLOSED == 31, "closed bit must sit directly below attempts");

constexpr uint64_t MASK_BASE     = (1ull << BITS_BASE) - 1;
constexpr uint64_t MASK_INITIAL  = (1ull << BITS_INITIAL) - 1;
constexpr uint64_t MASK_EPOCH    = (1ull << BITS_EPOCH) - 1;
constexpr uint64_t CLOSED_BIT    = 1ull << SHIFT_CLOSED;

constexpr uint32_t MAX_EPOCHS    = 1u << BITS_EPOCH;
constexpr uint32_t MAX_INITIAL   = (1u << BITS_INITIAL) - 1;
constexpr uint32_t BASE_SPAN     = 1u << BITS_BASE;

struct Fields {
  uint32_t attempts;
  bool     closed;
  uint32_t epoch;
  uint32_t initial;
  uint32_t base;
};

GPUTC_HD constexpr uint32_t attempts_of(uint64_t w) { return uint32_t(w >> ATTEMPT_SHIFT); }
GPUTC_HD constexpr bool     closed_of(uint64_t w)   { return (w & CLOSED_BIT) != 0; }
GPUTC_HD constexpr uint32_t epoch_of(uint64_t w)    { return uint32_t((w >> SHIFT_EPOCH) & MASK_EPOCH); }
GPUTC_HD constexpr uint32_t initial_of(uint64_t w)  { return uint32_t((w >> SHIFT_INITIAL) & MASK_INITIAL); }
GPUTC_HD constexpr uint32_t base_of(uint64_t w)     { return uint32_t((w >> SHIFT_BASE) & MASK_BASE); }

GPUTC_HD constexpr uint64_t pack(Fields f) {
  return (uint64_t(f.attempts) << ATTEMPT_SHIFT)
       | (f.closed ? CLOSED_BIT : 0)
       | ((uint64_t(f.epoch) & MASK_EPOCH) << SHIFT_EPOCH)
       | ((uint64_t(f.initial) & MASK_INITIAL) << SHIFT_INITIAL)
       | ((uint64_t(f.base) & MASK_BASE) << SHIFT_BASE);
}

GPUTC_HD constexpr Fields unpack(uint64_t w) {
  return Fields{attempts_of(w), closed_of(w), epoch_of(w), initial_of(w), base_of(w)};
}

GPUTC_HD constexpr uint64_t closed_word() { return CLOSED_BIT; }

GPUTC_HD constexpr uint64_t claim_ticket_increment() { return 1ull << ATTEMPT_SHIFT; }

// ---- geometric half-steal (chunks) ----

GPUTC_HD constexpr uint32_t half_remaining(uint32_t initial, uint32_t tickets) {
  if (initial == 0) return 0;
  const uint32_t h = bit_width(initial - 1);
  if (tickets > h) return 0;
  return 1u + ((initial - 1u) >> tickets);
}

struct ChunkRange {
  uint32_t first;
  uint32_t count;
};

GPUTC_HD constexpr ChunkRange half_claim(uint32_t initial, uint32_t ticket) {
  const uint32_t before = half_remaining(initial, ticket);
  if (before == 0) return ChunkRange{initial, 0};
  const uint32_t after = half_remaining(initial, ticket + 1);
  return ChunkRange{initial - before, before - after};
}

GPUTC_HD constexpr uint32_t half_claimed_prefix(uint32_t initial, uint32_t tickets) {
  return initial - half_remaining(initial, tickets);
}

GPUTC_HD constexpr bool stealable(uint64_t w) {
  Fields f = unpack(w);
  return !f.closed && half_remaining(f.initial, f.attempts) > 0;
}

// Chunks covered by valid tickets at close time, from the pre-close word.
GPUTC_HD constexpr uint32_t taken_at_close(uint64_t pre_close) {
  Fields f = unpack(pre_close);
  return half_claimed_prefix(f.initial, f.attempts);
}

struct Ticket {
  bool     valid;
  uint32_t epoch;
  uint32_t first;
  uint32_t n;
  uint32_t base;
};

// One steal attempt: fetch_add(claim_ticket_increment()) returns `old_word`.
GPUTC_HD constexpr Ticket interpret_claim(uint64_t old_word) {
  Fields f = unpack(old_word);
  if (f.closed) return Ticket{false, f.epoch, 0, 0, f.base};
  ChunkRange cr = half_claim(f.initial, f.attempts);
  if (cr.count == 0) return Ticket{false, f.epoch, 0, 0, f.base};
  return Ticket{true, f.epoch, cr.first, cr.count, f.base};
}

GPUTC_HD constexpr bool steal_guard_tripped(uint64_t old_word) {
  return attempts_of(old_word) + 1 >= STEAL_ATTEMPT_GUARD;
}

} // namespace gputc
