#pragma once

// SawsRing: the one SAWS queue, used for per-warp deques in shared memory, the
// block's global ring, and host tests. It is a non-owning view over a
// RingState and slot storage placed wherever the caller chose.
//
// Layout, in free-running task positions (oldest to newest):
//
//   tail ...closed epochs... pub_base ...published epoch... split ...private... head
//
// [tail, pub_base) is claimed work that thieves may still be copying out.
// [pub_base, split) is the published epoch described by the steal word, in
// whole chunks. [split, head) is private to the owner.
//
// Every method is a protocol step performed by a single agent. Owner methods
// require exclusive ownership (one warp's lane 0, or whoever holds a lock);
// thief methods are lock-free. Moving task bodies is left to the caller, so a
// warp can copy them with all lanes between the protocol steps.

#include "common.hpp"
#include "steal_word.hpp"
#include "epoch.hpp"

namespace gputc {

template<int Epochs>
struct RingState {
  uint64_t steal_word;
  uint32_t tail;
  uint32_t pub_base;
  uint32_t split;
  uint32_t head;
  bool     published;
  EpochFifo<Epochs> epochs;

  // start is the first task position, a multiple of the chunk size.
  GPUTC_HD void init(uint32_t start = 0) {
    steal_word = closed_word();
    tail = pub_base = split = head = start;
    published = false;
    epochs.head = epochs.next = 0;
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

template<class Body, class Scope, uint32_t Chunk, uint32_t CapChunks, int Epochs>
struct SawsRing {
  using State = RingState<Epochs>;
  static constexpr uint32_t chunk    = Chunk;
  static constexpr uint32_t capacity = CapChunks * Chunk;  // in tasks
  static_assert(is_pow2(Chunk), "chunk must be a power of two");
  static_assert(valid_capacity(CapChunks), "ring capacity must be a power of two that fits the steal word");

  State* st;
  Body*  slots;

  GPUTC_HD SawsRing(State* state, Body* data) : st(state), slots(data) {}

  GPUTC_HD Body& operator[](uint32_t pos) const { return slots[pos & (capacity - 1)]; }

  // ---- owner-side queries ----

  GPUTC_HD uint32_t used() const { return st->head - st->tail; }
  GPUTC_HD uint32_t private_size() const { return st->head - st->split; }
  GPUTC_HD uint32_t public_chunks() const { return (st->split - st->pub_base) / chunk; }

  // ---- owner: private end ----

  // Reserve up to `want` slots at the head, but only if at least `min` fit
  // after reclaiming what it can. The caller writes [pos, pos + n); n == 0
  // means fewer than `min` slots were free.
  GPUTC_HD Range push_reserve(uint32_t want, uint32_t min) {
    if (used() + want > capacity) {
      roll();
      reclaim();
    }
    uint32_t n = min_u32(want, capacity - used());
    if (n == 0 || n < min) return Range{st->head, 0};
    Range r{st->head, n};
    st->head += n;
    return r;
  }

  // Remove up to max tasks from the head; the caller reads [pos, pos + n).
  GPUTC_HD Range pop(uint32_t max) {
    uint32_t n = min_u32(max, private_size());
    st->head -= n;
    return Range{st->head, n};
  }

  // Remove the k oldest chunks the owner holds so the caller can move them
  // elsewhere. Unclaimed public chunks come back first. The caller reads
  // [pos, pos + n) before its next push; n == 0 unless k chunks were held.
  GPUTC_HD Range take_oldest(uint32_t k) {
    reacquire();
    const uint32_t n = k * chunk;
    if (n == 0 || private_size() < n) return Range{st->split, 0};
    Range r{st->split, n};
    st->pub_base = st->split = st->split + n;
    reclaim();
    return r;
  }

  // ---- owner: epochs ----

  // Publish the k oldest private chunks. The unclaimed rest of the published
  // epoch is adjacent, so it is republished in the same new epoch without a copy.
  GPUTC_HD bool release(uint32_t k) {
    if (k == 0 || private_size() < k * chunk) return false;
    if (!can_release()) return false;
    close();
    st->split += k * chunk;
    publish();
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
    if (st->epochs.can_close_and_publish()) return true;
    reclaim();
    return st->epochs.can_close_and_publish();
  }

  // Free the space of closed epochs whose chunks have all been copied out.
  GPUTC_HD void reclaim() {
    auto& f = st->epochs;
    while (!f.empty()) {
      EpochRec& r = f.oldest();
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
  // Claimed chunks of the published epoch can only be reclaimed once it
  // closes, so a full ring closes it and republishes the unclaimed remainder.
  GPUTC_HD void roll() {
    if (!st->published || !st->epochs.can_close_and_publish()) return;
    close();
    publish();
  }

  GPUTC_HD void close() {
    if (!st->published) return;
    uint64_t old = Scope::fetch_or_acq_rel(&st->steal_word, CLOSED_BIT);
    st->published = false;
    uint32_t t = taken(old);
    if (t == 0) return;  // no ticket landed, the slot is reused as is
    auto& f = st->epochs;
    EpochRec& r = f.published();
    r.taken = t;
    r.end = st->pub_base + t * chunk;
    st->pub_base = r.end;
    ++f.next;
  }

  GPUTC_HD void publish() {
    uint32_t avail = public_chunks();
    if (avail == 0) return;
    auto& f = st->epochs;
    Scope::store_relaxed(&f.published().retired, 0u);
    uint64_t w = pack(Fields{0, false, EpochFifo<Epochs>::slot(f.next), avail, st->pub_base / chunk});
    Scope::store_release(&st->steal_word, w);
    st->published = true;
  }
};

} // namespace gputc
