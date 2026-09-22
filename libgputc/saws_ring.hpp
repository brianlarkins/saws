#pragma once

// SawsRing: the one SAWS queue, used for per-warp deques in shared memory, the
// block's global ring, and host tests. It is a non-owning view over a
// RingState and slot storage placed wherever the caller chose.
//
// Layout, in free-running task positions (oldest to newest):
//
//   tail ...closed epochs... pub_base ...open epoch... split ...private... head
//
// [tail, pub_base) is claimed work that thieves may still be copying out.
// [pub_base, split) is the open epoch described by the steal word, in whole
// chunks. [split, head) is private to the owner.
//
// Every method is a protocol step performed by a single agent. Owner methods
// require exclusive ownership (one warp's lane 0, or whoever holds a lock);
// thief methods are lock-free. Moving task bodies is left to the caller, so a
// warp can copy them with all lanes between the protocol steps.

#include "common.hpp"
#include "StealWord.hpp"
#include "ring.hpp"
#include "epoch.hpp"

namespace gputc {

template<int Epochs>
struct RingState {
  uint64_t steal_word;
  uint32_t tail;
  uint32_t pub_base;
  uint32_t split;
  uint32_t head;
  uint32_t is_open;
  EpochFifo<Epochs> epochs;

  GPUTC_HD void init() {
    steal_word = closed_word();
    tail = pub_base = split = head = 0;
    is_open = 0;
    epochs.head = epochs.open = 0;
    for (int i = 0; i < Epochs; ++i)
      epochs.rec[i] = EpochRec{0, 0, 0};
  }
};

// Capacities are powers of two so positions can wrap freely, small enough that
// avail fits its field, and dividing the steal word's base span.
GPUTC_HD constexpr bool valid_capacity(uint32_t cap_chunks) {
  return is_pow2(cap_chunks) && cap_chunks <= MAX_AVAIL && cap_chunks <= BASE_SPAN;
}

struct Range {
  uint32_t pos;
  uint32_t n;
};

template<class Body, class Scope, class Cfg>
struct SawsRing {
  using State = RingState<Cfg::epochs>;
  static constexpr uint32_t chunk = Cfg::chunk;
  static_assert(is_pow2(chunk), "chunk must be a power of two");

  State*         st;
  RingView<Body> slots;

  GPUTC_HD SawsRing(State* state, Body* data, uint32_t cap_chunks)
      : st(state), slots{data, cap_chunks * chunk} {}

  GPUTC_HD Body& operator[](uint32_t pos) const { return slots[pos]; }

  // ---- owner-side queries ----

  GPUTC_HD uint32_t capacity() const { return slots.capacity; }
  GPUTC_HD uint32_t used() const { return st->head - st->tail; }
  GPUTC_HD uint32_t private_size() const { return st->head - st->split; }
  GPUTC_HD uint32_t public_chunks() const { return (st->split - st->pub_base) / chunk; }

  // ---- owner: private end ----

  // Reserve n slots at the head; the caller writes [pos, pos + n).
  GPUTC_HD bool try_push_reserve(uint32_t n, uint32_t& pos) {
    if (used() + n > capacity()) {
      roll();
      reclaim();
      if (used() + n > capacity()) return false;
    }
    pos = st->head;
    st->head += n;
    return true;
  }

  // Remove up to max tasks from the head; the caller reads [pos, pos + n).
  GPUTC_HD Range pop(uint32_t max) {
    uint32_t n = min_u32(max, private_size());
    st->head -= n;
    return Range{st->head, n};
  }

  // ---- owner: epochs ----

  // Publish the k oldest private chunks. The unclaimed rest of the open epoch
  // is adjacent, so it is republished in the same new epoch without a copy.
  GPUTC_HD bool release(uint32_t k) {
    if (k == 0 || private_size() < k * chunk) return false;
    if (!can_release()) return false;
    close();
    st->split += k * chunk;
    open();
    return true;
  }

  // Pull every unclaimed public chunk back to the private end.
  GPUTC_HD uint32_t reacquire() {
    close();
    uint32_t n = public_chunks();
    st->split = st->pub_base;
    return n;
  }

  // An epoch slot is free for the next release (reclaiming if needed).
  GPUTC_HD bool can_release() {
    if (st->epochs.can_close_and_reopen()) return true;
    reclaim();
    return st->epochs.can_close_and_reopen();
  }

  // Free the space of closed epochs whose chunks have all been copied out.
  GPUTC_HD void reclaim() {
    auto& f = st->epochs;
    while (!f.empty()) {
      EpochRec& r = f.front();
      if (Scope::load_acquire(&r.retired) != r.taken) break;
      st->tail = r.end;
      ++f.head;
    }
    if (f.empty()) st->tail = st->pub_base;
  }

  // ---- thief ----

  GPUTC_HD uint64_t probe() const { return Scope::load_relaxed(&st->steal_word); }

  GPUTC_HD Ticket claim(uint32_t n) {
    uint64_t old = Scope::fetch_add_acq_rel(&st->steal_word, claim_increment(n));
    return interpret(old, n);
  }

  // Task position of the first task of chunk j of a ticket.
  GPUTC_HD uint32_t chunk_pos(const Ticket& t, uint32_t j) const {
    return ((t.base + t.first + j) & (BASE_SPAN - 1)) * chunk;
  }

  // Report the copy-out complete; the owner may then reuse those slots.
  GPUTC_HD void retire(const Ticket& t) {
    Scope::fetch_add_release(&st->epochs.rec[t.epoch].retired, t.n);
  }

private:
  // Claimed chunks of the open epoch can only be reclaimed once it closes, so a
  // full ring closes it and republishes the unclaimed remainder.
  GPUTC_HD void roll() {
    if (!st->is_open || !st->epochs.can_close_and_reopen()) return;
    close();
    open();
  }

  GPUTC_HD void close() {
    if (!st->is_open) return;
    uint64_t old = Scope::fetch_or_acq_rel(&st->steal_word, CLOSED_BIT);
    st->is_open = 0;
    uint32_t t = taken(old);
    if (t == 0) return;  // no ticket landed, the slot is reused as is
    auto& f = st->epochs;
    EpochRec& r = f.current();
    r.taken = t;
    r.end = st->pub_base + t * chunk;
    st->pub_base = r.end;
    ++f.open;
  }

  GPUTC_HD void open() {
    uint32_t avail = public_chunks();
    if (avail == 0) return;
    auto& f = st->epochs;
    Scope::store_relaxed(&f.current().retired, 0u);
    uint64_t w = pack(Fields{0, false, EpochFifo<Cfg::epochs>::slot(f.open), avail, st->pub_base / chunk});
    Scope::store_release(&st->steal_word, w);
    st->is_open = 1;
  }
};

} // namespace gputc
