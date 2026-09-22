#pragma once

// The per-warp worker. Protocol steps on a ring run on lane 0; every lane
// then moves one task body. A warp looks for work in cost order:
//
//   own private end -> reacquire own public -> sibling public ends
//     -> the block's global ring -> idle (termination check, backoff)
//
// Spawns go to the warp's private end. When it cannot fit a spawn, the warp
// promotes the top half of its private chunks into the global ring.

#include <cstdio>

#include "config.hpp"
#include "stats.hpp"
#include "atomics.hpp"
#include "saws_ring.hpp"
#include "warp.cuh"
#include "context.cuh"
#include "termination.cuh"

namespace gputc {

// Block-shared scheduler state, a static __shared__ object.
template<class Cfg>
struct Shared {
  RingState<Cfg::epochs>   warp_state[Cfg::warps];
  Termination<Cfg::warps>  term;
  Counters                 counters[Cfg::warps];
};

// The block's global ring, in device memory.
template<class Body, class Cfg>
struct GlobalRef {
  RingState<Cfg::epochs>* state;
  Body*                   slots;
  uint32_t*               lock;   // serializes the owner end between warps
};

template<class Task, class Cfg>
struct KernelArgs {
  using Body = typename Task::Body;
  GlobalRef<Body, Cfg> global;
  Counters*            out;
  Policy               policy;
  Body                 root;
};

template<class Task, class Cfg>
struct Worker {
  using Body       = typename Task::Body;
  using WarpRing   = SawsRing<Body, BlockScope, Cfg>;
  using GlobalRing = SawsRing<Body, DeviceScope, Cfg>;
  static constexpr uint32_t C = Cfg::chunk;
  static constexpr uint32_t warp_slots = Cfg::warp_chunks * C;
  static constexpr long long promote_spin_limit = 1ll << 22;

  Shared<Cfg>& sh;
  Body*        deques;   // warp_slots bodies per warp, dynamic shared memory
  WarpRing     mine;
  GlobalRing   global;
  uint32_t*    global_lock;
  Policy       policy;
  int          me;
  uint32_t     rng;      // warp-uniform
  int          backoff_ns;

  __device__ Worker(Shared<Cfg>& s, Body* d, const KernelArgs<Task, Cfg>& a)
      : sh(s), deques(d), mine(ring_of(s, d, warp::id())),
        global(a.global.state, a.global.slots, Cfg::global_chunks),
        global_lock(a.global.lock), policy(a.policy), me(warp::id()),
        rng(0x9E3779B9u * (warp::id() + 1)), backoff_ns(a.policy.backoff_min_ns) {}

  static __device__ WarpRing ring_of(Shared<Cfg>& s, Body* d, int w) {
    return WarpRing(&s.warp_state[w], d + w * warp_slots, Cfg::warp_chunks);
  }

  __device__ Counters& stats() { return sh.counters[me]; }

  __device__ uint32_t next_random() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }

  // ---- main loop ----

  __device__ void run() {
    for (;;) {
      Body body{};
      uint32_t n = 0;
      if (pop_own(body, n) || steal_sibling(body, n) || steal_global(body, n)) {
        execute(body, n);
        maybe_release();
        backoff_ns = policy.backoff_min_ns;
      } else if (idle()) {
        return;
      }
    }
  }

  __device__ void execute(const Body& body, uint32_t n) {
    Ctx<Worker> ctx{*this};
    Task::execute(ctx, body, uint32_t(warp::lane()) < n);
    if (warp::leader()) sh.term.add_executed(me, n);
  }

  // ---- finding work ----

  __device__ bool pop_own(Body& body, uint32_t& n) {
    Range r{0, 0};
    if (warp::leader()) {
      r = mine.pop(C);
      if (r.n == 0 && mine.reacquire() > 0) {
        stats().reacquires++;
        r = mine.pop(C);
      }
    }
    r = warp::bcast(r);
    if (r.n == 0) return false;
    if (uint32_t(warp::lane()) < r.n) body = mine[r.pos + warp::lane()];
    __syncwarp();  // all reads land before any lane's spawn reuses the slots
    n = r.n;
    return true;
  }

  __device__ bool steal_sibling(Body& body, uint32_t& n) {
    for (int attempt = 0; attempt < policy.probe_retries; ++attempt) {
      int l = warp::lane();
      bool ok = l < Cfg::warps && l != me
             && stealable(BlockScope::load_relaxed(&sh.warp_state[l].steal_word));
      unsigned mask = __ballot_sync(warp::FULL, ok);
      if (!mask) return false;
      int victim = warp::pick(mask, next_random() & (warp::SIZE - 1));
      WarpRing v = ring_of(sh, deques, victim);
      if (take(v, body)) {
        if (warp::leader()) stats().steals_sibling++;
        n = C;
        return true;
      }
    }
    return false;
  }

  __device__ bool steal_global(Body& body, uint32_t& n) {
    int ok = warp::leader() ? stealable(global.probe()) : 0;
    if (!__shfl_sync(warp::FULL, ok, 0)) return false;
    if (!take(global, body)) return false;
    if (warp::leader()) stats().steals_global++;
    n = C;
    return true;
  }

  // Claim one chunk from a ring and copy it lane-per-task into registers.
  template<class Ring>
  __device__ bool take(Ring& ring, Body& body) {
    Ticket t{};
    if (warp::leader()) {
      t = ring.claim(1);
      if (!t.valid) stats().steal_misses++;
    }
    __syncwarp();  // order lane 0's acquire before the other lanes' reads
    t = warp::bcast(t);
    if (!t.valid) return false;
    body = ring[ring.chunk_pos(t, 0) + warp::lane()];
    __syncwarp();
    if (warp::leader()) ring.retire(t);
    return true;
  }

  // ---- publishing work ----

  __device__ void maybe_release() {
    if (!warp::leader()) return;
    uint32_t full = mine.private_size() / C;
    if (int(full) < policy.release_min_chunks || stealable(mine.probe())) return;
    if (mine.release(full > 1 ? full / 2 : 1)) stats().releases++;
  }

  // ---- spawning ----

  __device__ void spawn(const Body& body, bool pred) {
    unsigned mask = __ballot_sync(warp::FULL, pred);
    if (!mask) return;
    uint32_t count = __popc(mask);
    uint32_t pos = reserve(count);
    if (pred) mine[pos + __popc(mask & warp::lanemask_lt())] = body;
    __syncwarp();
    if (warp::leader()) sh.term.add_spawned(me, count);
  }

  __device__ uint32_t reserve(uint32_t count) {
    for (;;) {
      int ok = 0;
      uint32_t pos = 0;
      if (warp::leader()) ok = mine.try_push_reserve(count, pos);
      if (__shfl_sync(warp::FULL, ok, 0)) return __shfl_sync(warp::FULL, pos, 0);
      make_room();
    }
  }

  // The deque is full. Move private chunks to the global ring if there are
  // any; otherwise the space is pinned by siblings mid-copy, so wait for them.
  __device__ void make_room() {
    uint32_t priv = 0;
    if (warp::leader()) {
      priv = mine.private_size();
      if (priv < C && mine.reacquire() > 0) {
        stats().reacquires++;
        priv = mine.private_size();
      }
    }
    priv = __shfl_sync(warp::FULL, priv, 0);
    if (priv >= C) {
      uint32_t full = priv / C;
      promote(full > 1 ? full / 2 : 1);
    } else {
      __nanosleep(policy.backoff_min_ns);
    }
  }

  // Move the top k private chunks into the global ring and publish them.
  __device__ void promote(uint32_t k) {
    const uint32_t need = k * C;
    uint32_t gpos = 0;
    if (warp::leader()) {
      while (!DeviceScope::cas_acquire(global_lock, 0u, 1u)) __nanosleep(64);
      for (long long spins = 0; !global.try_push_reserve(need, gpos); ++spins) {
        if (spins == promote_spin_limit) {
          printf("gputc: global ring full (%u chunks), raise Cfg::global_chunks\n", Cfg::global_chunks);
          __trap();
        }
        __nanosleep(256);
      }
      // Reserving may roll the open epoch; free slots return as thieves retire.
      while (!global.can_release()) __nanosleep(64);
    }
    gpos = __shfl_sync(warp::FULL, gpos, 0);
    Range r{0, 0};
    if (warp::leader()) r = mine.pop(need);
    r = warp::bcast(r);
    for (uint32_t i = warp::lane(); i < need; i += warp::SIZE) global[gpos + i] = mine[r.pos + i];
    __syncwarp();
    if (warp::leader()) {
      DeviceScope::fence_release();
      global.release(k);
      DeviceScope::store_release(global_lock, 0u);
      stats().promotes++;
    }
    __syncwarp();
  }

  // ---- idling ----

  __device__ bool idle() {
    int done = warp::leader() ? sh.term.finished() : 0;
    if (__shfl_sync(warp::FULL, done, 0)) return true;
    __nanosleep(backoff_ns);
    backoff_ns = min(backoff_ns * 2, policy.backoff_max_ns);
    return false;
  }

  // ---- lifecycle ----

  __device__ void seed(const Body& root) { spawn(root, warp::leader()); }

  __device__ void flush(Counters* out) {
    if (!warp::leader()) return;
    Counters& c = stats();
    atomicAdd(&out->executed, sh.term.executed[me]);
    atomicAdd(&out->spawned, sh.term.spawned[me]);
    atomicAdd(&out->steals_sibling, c.steals_sibling);
    atomicAdd(&out->steals_global, c.steals_global);
    atomicAdd(&out->steal_misses, c.steal_misses);
    atomicAdd(&out->releases, c.releases);
    atomicAdd(&out->reacquires, c.reacquires);
    atomicAdd(&out->promotes, c.promotes);
  }
};

} // namespace gputc
