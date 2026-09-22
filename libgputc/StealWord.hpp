#pragma once

// The 64-bit steal word. All counts are in chunks.
//
//   63            32   31    30   27  26      14  13      0
//  +----------------+-------+-------+-----------+----------+
//  |    claimed     | closed| epoch |   avail   |   base   |
//  +----------------+-------+-------+-----------+----------+
//
// claimed must be the high half: thieves fetch_add (n << CLAIM_SHIFT), and a
// carry out of bit 63 is discarded instead of corrupting the other fields.
// Overclaim is bounded by probing (stealable()) before claiming, and every
// newly published epoch starts again at claimed = 0.

#include "common.hpp"

namespace gputc {

constexpr int BITS_BASE  = 14;
constexpr int BITS_AVAIL = 13;
constexpr int BITS_EPOCH = 4;

constexpr int SHIFT_BASE  = 0;
constexpr int SHIFT_AVAIL = SHIFT_BASE + BITS_BASE;
constexpr int SHIFT_EPOCH = SHIFT_AVAIL + BITS_AVAIL;
constexpr int SHIFT_CLOSED = SHIFT_EPOCH + BITS_EPOCH;
constexpr int CLAIM_SHIFT = 32;

static_assert(SHIFT_CLOSED == 31, "closed bit must sit directly below claimed");

constexpr uint64_t MASK_BASE  = (1ull << BITS_BASE) - 1;
constexpr uint64_t MASK_AVAIL = (1ull << BITS_AVAIL) - 1;
constexpr uint64_t MASK_EPOCH = (1ull << BITS_EPOCH) - 1;
constexpr uint64_t CLOSED_BIT = 1ull << SHIFT_CLOSED;

constexpr uint32_t MAX_EPOCHS = 1u << BITS_EPOCH;
constexpr uint32_t MAX_AVAIL  = (1u << BITS_AVAIL) - 1;
constexpr uint32_t BASE_SPAN  = 1u << BITS_BASE;

struct Fields {
  uint32_t claimed;
  bool     closed;
  uint32_t epoch;
  uint32_t avail;
  uint32_t base;
};

GPUTC_HD constexpr uint64_t pack(Fields f) {
  return (uint64_t(f.claimed) << CLAIM_SHIFT)
       | (f.closed ? CLOSED_BIT : 0)
       | ((uint64_t(f.epoch) & MASK_EPOCH) << SHIFT_EPOCH)
       | ((uint64_t(f.avail) & MASK_AVAIL) << SHIFT_AVAIL)
       | ((uint64_t(f.base)  & MASK_BASE)  << SHIFT_BASE);
}

GPUTC_HD constexpr Fields unpack(uint64_t w) {
  return Fields{
    uint32_t(w >> CLAIM_SHIFT),
    (w & CLOSED_BIT) != 0,
    uint32_t((w >> SHIFT_EPOCH) & MASK_EPOCH),
    uint32_t((w >> SHIFT_AVAIL) & MASK_AVAIL),
    uint32_t((w >> SHIFT_BASE)  & MASK_BASE),
  };
}

GPUTC_HD constexpr uint64_t closed_word() { return CLOSED_BIT; }

GPUTC_HD constexpr uint64_t claim_increment(uint32_t n) { return uint64_t(n) << CLAIM_SHIFT; }

// Probe: is a claim against this word worth issuing?
GPUTC_HD constexpr bool stealable(uint64_t w) {
  return !(w & CLOSED_BIT)
      && uint32_t(w >> CLAIM_SHIFT) < uint32_t((w >> SHIFT_AVAIL) & MASK_AVAIL);
}

// Chunks of the epoch that belong to valid tickets, from the word returned by
// the owner's close (fetch_or). Close and claims linearize on the same word.
GPUTC_HD constexpr uint32_t taken(uint64_t pre_close) {
  return min_u32(uint32_t(pre_close >> CLAIM_SHIFT),
                 uint32_t((pre_close >> SHIFT_AVAIL) & MASK_AVAIL));
}

// A thief's share: ticket k owns chunk k. `first` is relative to the epoch, so
// the ring position of chunk j of the share is base + first + j (mod ring).
struct Ticket {
  bool     valid;
  uint32_t epoch;
  uint32_t first;
  uint32_t n;
  uint32_t base;
};

// Meaning of the pre-increment word returned by fetch_add(claim_increment(n)).
GPUTC_HD constexpr Ticket interpret(uint64_t old_word, uint32_t n) {
  Fields f = unpack(old_word);
  if (f.closed || f.claimed >= f.avail || n == 0)
    return Ticket{false, f.epoch, 0, 0, f.base};
  return Ticket{true, f.epoch, f.claimed, min_u32(n, f.avail - f.claimed), f.base};
}

} // namespace gputc
