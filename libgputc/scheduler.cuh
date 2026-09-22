#pragma once

// The per-warp worker. Protocol steps on a ring run on lane 0 (warp::lead);
// every lane then moves one task body. A warp looks for work in cost order:
//
//   own private end -> reacquire own public -> sibling public ends
//     -> the block's global ring -> idle (termination check, backoff)
//
// Spawns go to the warp's private end. When a spawn does not fit, the warp
// spills the oldest half of its chunks onto the global ring, and warps that
// run dry take the newest chunk back off it. The global ring thus extends the
// bottom of the warps' stacks, and the block's traversal stays depth-first,
// which is what bounds its memory. (Taking the oldest, as a thief does, makes
// the ring a FIFO whose frontier grows breadth-first.)
//
// In V1 the global ring's steal word is never published: with one block there
// are no thieves outside the block. Its public end is for other blocks.

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
  RingState<Cfg::warp_epochs> warp_state[Cfg::warps];
  Termination<Cfg::warps>     term;
  WarpCounters                counters[Cfg::warps];
};

// The global ring's control block, in device memory.
template<class Cfg>
struct GlobalControl {
  RingState<Cfg::global_epochs> ring;
  uint32_t lock;    // serializes the owner end between warps
  uint32_t chunks;  // chunks held, written under the lock, probed without it
  uint32_t peak;    // most tasks held, updated under the lock
};

template<class Task, class Cfg>
struct KernelArgs {
  using Body = typename Task::Body;
  GlobalControl<Cfg>* global;
  Body*               global_slots;
  WarpCounters*       out;  // one per warp
  Policy              policy;
  Body                root;
};

template<class Task, class Cfg>
struct Worker {
  using Body = typename Task::Body;
  static constexpr uint32_t C = Cfg::chunk;
  using WarpRing   = SawsRing<Body, BlockScope, C, Cfg::warp_chunks, Cfg::warp_epochs>;
  using GlobalRing = SawsRing<Body, DeviceScope, C, Cfg::global_chunks, Cfg::global_epochs>;

  Shared<Cfg>&        sh;
  Body*               deques;    // WarpRing::capacity bodies per warp, dynamic shared memory
  WarpRing            mine;
  GlobalRing          global;
  GlobalControl<Cfg>* gctl;
  Policy              policy;
  int                 me;
  uint32_t            rng;       // warp-uniform
  int                 backoff_ns;
  int                 phase;     // Phase being charged
  long long           phase_t0;  // lane 0: clock at the last phase change
  long long           start;

  __device__ Worker(Shared<Cfg>& s, Body* d, const KernelArgs<Task, Cfg>& a)
      : sh(s), deques(d), mine(ring_of(s, d, warp::id())),
        global(&a.global->ring, a.global_slots), gctl(a.global),
        policy(a.policy), me(warp::id()),
        rng(0x9E3779B9u * (warp::id() + 1)), backoff_ns(a.policy.backoff_min_ns),
        phase(PHASE_IDLE), phase_t0(warp::clock()), start(phase_t0) {}

  static __device__ WarpRing ring_of(Shared<Cfg>& s, Body* d, int w) {
    return WarpRing(&s.warp_state[w], d + w * WarpRing::capacity);
  }

  __device__ WarpCounters& stats() { return sh.counters[me]; }

  __device__ uint32_t next_random() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }

  // ---- time accounting ----

  // Charge the cycles since the last change to the current phase and switch
  // to p, returning the phase it replaced. Only lane 0's clock is recorded.
  __device__ int enter(int p) {
    int prev = phase;
    if constexpr (Cfg::timing) {
      if (warp::leader()) {
        long long now = warp::clock();
        stats().cycles[phase] += now - phase_t0;
        phase_t0 = now;
      }
    }
    phase = p;
    return prev;
  }

  // Charges a scope to a phase, then returns to the enclosing one.
  struct Timed {
    Worker& w;
    int     prev;
    __device__ Timed(Worker& worker, int p) : w(worker), prev(worker.enter(p)) {}
    __device__ ~Timed() { w.enter(prev); }
  };

  // ---- main loop ----

  __device__ void run() {
    for (;;) {
      Body body{};
      uint32_t n = 0;
      if (pop_own(body, n) || steal_sibling(body, n) || pop_global(body, n)) {
        execute(body, n);
        maybe_release();
        backoff_ns = policy.backoff_min_ns;
      } else if (idle()) {
        return;
      }
    }
  }

  __device__ void execute(const Body& body, uint32_t n) {
    Timed t(*this, PHASE_TASK);
    Ctx<Worker> ctx{*this};
    Task::execute(ctx, body, uint32_t(warp::lane()) < n);
    if (warp::leader()) {
      sh.term.add_executed(me, n);
      stats().chunks++;
    }
  }

  // ---- finding work ----

  __device__ bool pop_own(Body& body, uint32_t& n) {
    Timed t(*this, PHASE_POP);
    Range r = warp::lead([&] {
      Range got = mine.pop(C);
      if (got.n == 0 && mine.reacquire() > 0) {
        stats().reacquires++;
        got = mine.pop(C);
      }
      if (got.n > 0) stats().pops++;
      return got;
    });
    if (r.n == 0) return false;
    if (uint32_t(warp::lane()) < r.n) body = warp::load_body(&mine[r.pos + warp::lane()]);
    __syncwarp();  // all reads land before any lane's spawn reuses the slots
    n = r.n;
    return true;
  }

  __device__ bool steal_sibling(Body& body, uint32_t& n) {
    Timed t(*this, PHASE_STEAL_SIBLING);
    for (int attempt = 0; attempt < policy.probe_retries; ++attempt) {
      int l = warp::lane();
      bool ok = l < Cfg::warps && l != me
             && stealable(BlockScope::load_relaxed(&sh.warp_state[l].steal_word));
      unsigned mask = __ballot_sync(warp::FULL, ok);
      if (warp::leader()) stats().sibling_probes++;
      if (!mask) return false;
      WarpRing victim = ring_of(sh, deques, warp::pick_random(mask, next_random()));
      if (warp::leader()) stats().sibling_claims++;
      if (take(victim, body)) {
        if (warp::leader()) stats().sibling_steals++;
        n = C;
        return true;
      }
    }
    return false;
  }

  // Claim one chunk from a ring and copy it lane-per-task into registers.
  template<class Ring>
  __device__ bool take(Ring& ring, Body& body) {
    Ticket t = warp::lead([&] { return ring.claim(1); });
    if (!t.valid) return false;
    body = warp::load_body(&ring[ring.chunk_pos(t, 0) + warp::lane()]);
    __syncwarp();  // every lane's read lands before the slots are handed back
    if (warp::leader()) ring.retire(t);
    return true;
  }

  // Take the newest chunk of the global ring.
  __device__ bool pop_global(Body& body, uint32_t& n) {
    Timed t(*this, PHASE_GLOBAL);
    Range r = warp::lead([&] {
      stats().global_probes++;
      if (DeviceScope::load_relaxed(&gctl->chunks) == 0) return Range{0, 0};
      lock_global();
      Range got = global.pop(C);
      if (got.n == 0) unlock_global();
      return got;
    });
    if (r.n == 0) return false;
    if (uint32_t(warp::lane()) < r.n) body = warp::load_body(&global[r.pos + warp::lane()]);
    __syncwarp();  // every lane's read lands before the lock lets the slots be reused
    if (warp::leader()) {
      unlock_global();
      stats().global_pops++;
    }
    n = r.n;
    return true;
  }

  // ---- publishing work ----

  __device__ void maybe_release() {
    Timed t(*this, PHASE_RELEASE);
    if (!warp::leader()) return;
    uint32_t full = mine.private_size() / C;
    if (int(full) < policy.release_min_chunks || stealable(mine.probe())) return;
    uint32_t k = full > 1 ? full / 2 : 1;
    if (mine.release(k)) {
      stats().releases++;
      stats().released_chunks += k;
    }
  }

  // ---- spawning ----

  __device__ void spawn(const Body& body, bool pred) {
    unsigned mask = __ballot_sync(warp::FULL, pred);
    if (!mask) return;
    Timed t(*this, PHASE_SPAWN);
    uint32_t count = __popc(mask);
    Range r = reserve(count, count);
    if (pred) warp::store_body(&mine[r.pos + __popc(mask & warp::lanemask_lt())], body);
    __syncwarp();
    count_spawned(r.n, 1);
  }

  // Lane l spawns make(parent_l, 0..count_l-1). The warp's children are
  // numbered lane-major, and each round every lane builds the next one,
  // reading its parent from the owning lane.
  template<class Parent, class Make>
  __device__ void spawn_n(uint32_t count, const Parent& parent, Make& make) {
    const uint32_t inclusive = warp::inclusive_sum(count);
    const uint32_t first = inclusive - count;
    const uint32_t total = __shfl_sync(warp::FULL, inclusive, warp::SIZE - 1);
    for (uint32_t done = 0; done < total;) {
      Range r;
      {
        Timed t(*this, PHASE_SPAWN);
        r = reserve(total - done, min_u32(total - done, C));
      }
      uint32_t rounds = 0;
      for (uint32_t base = 0; base < r.n; base += warp::SIZE, ++rounds) {
        const uint32_t slot = base + warp::lane();
        const uint32_t g = done + min_u32(slot, r.n - 1);
        const int owner = warp::owner_of(inclusive, g);
        const Parent p = warp::shfl(parent, owner);
        const uint32_t i = g - __shfl_sync(warp::FULL, first, owner);
        if (slot < r.n) warp::store_body(&mine[r.pos + slot], make(p, int(i)));
      }
      __syncwarp();
      count_spawned(r.n, rounds);
      done += r.n;
    }
  }

  // Counted after the bodies are written and before any other warp can see
  // them, which the termination check relies on.
  __device__ void count_spawned(uint32_t n, uint32_t rounds) {
    if (warp::leader()) {
      sh.term.add_spawned(me, n);
      stats().spawn_rounds += rounds;
    }
  }

  // Reserve at least min and up to want deque slots, making room as needed.
  __device__ Range reserve(uint32_t want, uint32_t min) {
    for (;;) {
      Range r = warp::lead([&] {
        Range got = mine.push_reserve(want, min);
        if (got.n > 0 && mine.used() > stats().deque_peak) stats().deque_peak = mine.used();
        return got;
      });
      if (r.n > 0) return r;
      make_room();
    }
  }

  // The deque is full. Spill the oldest half of the chunks this warp still
  // holds; if it holds none, the space is pinned by siblings mid-copy, so wait.
  __device__ void make_room() {
    Timed t(*this, PHASE_SPILL);
    uint32_t full = warp::lead([&] {
      if (mine.reacquire() > 0) stats().reacquires++;
      return mine.private_size() / C;
    });
    if (full == 0) {
      if (warp::leader()) stats().pinned_waits++;
      __nanosleep(policy.pinned_wait_ns);
      return;
    }
    spill(full > 1 ? full / 2 : 1);
  }

  // Move this warp's k oldest chunks to the top of the global ring.
  __device__ void spill(uint32_t k) {
    const uint32_t n = k * C;
    uint32_t gpos = warp::lead([&] {
      lock_global();
      Range got = global.push_reserve(n, n);
      GPUTC_CHECK(got.n == n, "global ring full; raise Cfg::global_chunks");
      return got.pos;
    });
    Range r = warp::lead([&] { return mine.take_oldest(k); });
    for (uint32_t j = 0; j < k; ++j) warp::copy_bodies<C>(&global[gpos + j * C], &mine[r.pos + j * C]);
    __syncwarp();
    if (warp::leader()) {
      GPUTC_CHECK(r.n == n, "spill found fewer chunks than it reserved");
      gctl->peak = max_u32(gctl->peak, global.used());
      unlock_global();
      stats().spills++;
      stats().spilled_chunks += k;
    }
  }

  // Lane 0 only. The lock orders every owner-side access to the global ring.
  __device__ void lock_global() {
    long long t0 = warp::clock();
    while (!DeviceScope::cas_acquire(&gctl->lock, 0u, 1u)) __nanosleep(policy.lock_backoff_ns);
    stats().lock_cycles += warp::clock() - t0;
  }

  __device__ void unlock_global() {
    DeviceScope::store_relaxed(&gctl->chunks, global.used() / C);
    DeviceScope::store_release(&gctl->lock, 0u);
  }

  // ---- idling ----

  __device__ bool idle() {
    Timed t(*this, PHASE_IDLE);
    bool done = warp::lead([&] {
      stats().idle_rounds++;
      return sh.term.finished();
    });
    if (done) return true;
    __nanosleep(backoff_ns);
    backoff_ns = min(backoff_ns * 2, policy.backoff_max_ns);
    return false;
  }

  // ---- lifecycle ----

  __device__ void seed(const Body& root) { spawn(root, warp::leader()); }

  __device__ void finish(WarpCounters* out) {
    enter(PHASE_IDLE);  // charge the phase still open
    if (!warp::leader()) return;
    WarpCounters& c = stats();
    c.tasks = sh.term.executed[me];
    c.spawned = sh.term.spawned[me];
    c.total_cycles = (Cfg::timing ? phase_t0 : warp::clock()) - start;
    out[me] = c;
  }
};

} // namespace gputc
