#pragma once

// SawsRing: one SAWS deque per worker in global memory (V2). Non-owning view over
// a RingState and a slice of a body slab.
//
//   tail ...closed epochs... pub_base ...published epoch... split ...private... head
//
// Owner methods run on lane 0; thief claim/retire are lock-free.

#include "atomics.hpp"
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

  GPUTC_HD void init(uint32_t start = 0) {
    steal_word = closed_word();
    tail = pub_base = split = head = start;
    published = false;
    epochs.head = epochs.next = 0;
    for (int i = 0; i < Epochs; ++i)
      epochs.rec[i] = EpochRec{0, 0, 0};
  }
};

GPUTC_HD constexpr bool valid_capacity(uint32_t cap_chunks) {
  return is_pow2(cap_chunks) && cap_chunks <= MAX_INITIAL && cap_chunks <= BASE_SPAN;
}

struct Range {
  uint32_t pos;
  uint32_t n;
};

enum class ReserveStatus : uint8_t {
  Ok,
  Retry,   // pinned copies may free space
  Full,    // owned work + reservation exceeds capacity
};

enum class AcquireStatus : uint8_t {
  HasPrivate,  // private tasks available (including after a successful pull)
  NoPublic,    // no published work to pull
  Retry,       // epoch bookkeeping temporarily blocked
};

template<class Body, class Scope, uint32_t Chunk, uint32_t CapChunks, int Epochs>
struct SawsRing {
  using State = RingState<Epochs>;
  static constexpr uint32_t chunk      = Chunk;
  static constexpr uint32_t capacity   = CapChunks * Chunk;
  static constexpr uint32_t max_steal_chunks = max_u32(1u, CapChunks / 2);
  static_assert(is_pow2(Chunk), "chunk must be a power of two");
  static_assert(valid_capacity(CapChunks), "ring capacity must fit the steal word");

  State* st;
  Body*  slots;

  GPUTC_HD SawsRing(State* state, Body* data) : st(state), slots(data) {}

  GPUTC_HD Body& operator[](uint32_t pos) const { return slots[pos & (capacity - 1)]; }

  GPUTC_HD uint32_t used() const { return st->head - st->tail; }
  GPUTC_HD uint32_t private_size() const { return st->head - st->split; }
  GPUTC_HD uint32_t public_chunks() const { return (st->split - st->pub_base) / chunk; }

  GPUTC_HD uint32_t unclaimed_public_chunks() const {
    if (!st->published) return 0;
    uint64_t w = Scope::load_relaxed(&st->steal_word);
    return half_remaining(initial_of(w), attempts_of(w));
  }

  GPUTC_HD uint32_t owned_tasks() const {
    return private_size() + unclaimed_public_chunks() * chunk;
  }

  // ---- owner: private end ----

  GPUTC_HD ReserveStatus push_reserve(uint32_t want, uint32_t min, Range* out) {
    reclaim();
    if (used() + min > capacity) roll();
    if (owned_tasks() + min > capacity) return ReserveStatus::Full;
    if (used() + min > capacity) return ReserveStatus::Retry;
    uint32_t n = min_u32(want, capacity - used());
    if (n < min) return ReserveStatus::Retry;
    *out = Range{st->head, n};
    st->head += n;
    return ReserveStatus::Ok;
  }

  GPUTC_HD Range pop(uint32_t max) {
    uint32_t n = min_u32(max, private_size());
    st->head -= n;
    return Range{st->head, n};
  }

  // Space for an incoming steal of up to max_steal_chunks (conservative bound).
  GPUTC_HD ReserveStatus incoming_reserve(uint32_t steal_chunks, Range* dest) {
    reclaim();
    const uint32_t need = steal_chunks * chunk;
    if (used() + need > capacity) roll();
    if (owned_tasks() + need > capacity) return ReserveStatus::Full;
    if (used() + need > capacity) return ReserveStatus::Retry;
    *dest = Range{st->head, need};
    st->head += need;
    return ReserveStatus::Ok;
  }

  // ---- owner: half release / acquire ----

  // Publish ~half of the oldest private chunks when the current epoch is exhausted.
  // Chunks published (0 if nothing changed).
  GPUTC_HD uint32_t release_half() {
    const uint32_t full = private_size() / chunk;
    if (full == 0) return 0;
    if (st->published && unclaimed_public_chunks() > 0) return 0;
    if (!can_release()) return 0;
    if (st->published) close();
    const uint32_t k = ceil_div_u32(full, 2);
    st->split += k * chunk;
    publish();
    return k;
  }

  GPUTC_HD AcquireStatus acquire_half_status() {
    if (private_size() > 0) return AcquireStatus::HasPrivate;
    if (!st->published) return AcquireStatus::NoPublic;
    if (!st->epochs.can_close_and_publish()) {
      reclaim();
      if (!st->epochs.can_close_and_publish()) return AcquireStatus::Retry;
    }
    close();
    const uint32_t R = public_chunks();
    if (R == 0) return AcquireStatus::NoPublic;
    const uint32_t reacq = ceil_div_u32(R, 2);
    const uint32_t pub   = R / 2;
    st->split -= reacq * chunk;
    if (pub > 0) publish(pub);
    return reacq > 0 ? AcquireStatus::HasPrivate : AcquireStatus::NoPublic;
  }

  GPUTC_HD bool can_release() {
    if (st->epochs.can_close_and_publish()) return true;
    reclaim();
    return st->epochs.can_close_and_publish();
  }

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

  GPUTC_HD void progress() { reclaim(); }

  // ---- thief ----

  GPUTC_HD uint64_t probe() const { return Scope::load_relaxed(&st->steal_word); }

  GPUTC_HD Ticket claim(uint64_t* pre_increment = nullptr) {
    uint64_t old = Scope::fetch_add_acq_rel(&st->steal_word, claim_ticket_increment());
    if (pre_increment) *pre_increment = old;
    return interpret_claim(old);
  }

  GPUTC_HD uint32_t chunk_pos(const Ticket& t, uint32_t j) const {
    return ((t.base + t.first + j) & (BASE_SPAN - 1)) * chunk;
  }

  GPUTC_HD void retire(const Ticket& t) {
    Scope::fetch_add_release(&st->epochs.rec[t.epoch].retired, t.n);
  }

private:
  GPUTC_HD void roll() {
    if (!st->published || !st->epochs.can_close_and_publish()) return;
    close();
    if (public_chunks() > 0) publish();
  }

  GPUTC_HD void close() {
    if (!st->published) return;
    uint64_t old = Scope::fetch_or_acq_rel(&st->steal_word, CLOSED_BIT);
    st->published = false;
    const uint32_t t = taken_at_close(old);
    if (t == 0) return;
    auto& f = st->epochs;
    EpochRec& r = f.published();
    r.taken = t;
    r.end = st->pub_base + t * chunk;
    st->pub_base = r.end;
    ++f.next;
  }

  GPUTC_HD void publish() { publish(public_chunks()); }

  GPUTC_HD void publish(uint32_t avail) {
    if (avail == 0) return;
    auto& f = st->epochs;
    Scope::store_relaxed(&f.published().retired, 0u);
    uint64_t w = pack(Fields{0, false, EpochFifo<Epochs>::slot(f.next), avail, st->pub_base / chunk});
    Scope::store_release(&st->steal_word, w);
    st->published = true;
  }
};

} // namespace gputc
