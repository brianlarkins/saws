#pragma once

#include "config.hpp"
#include "stats.hpp"
#include "saws_ring.hpp"
#include "warp.cuh"
#include "context.cuh"
#include "termination.cuh"

namespace gputc {

template<class Task, class Cfg>
struct Worker {
  using Body = typename Task::Body;
  static constexpr uint32_t C = Cfg::chunk;
  using Ring = SawsRing<Body, C, Cfg::deque_chunks, Cfg::epochs>;

  KernelArgs<Task, Cfg> args;
  TerminationView       term;
  Ring                  mine;
  int                   worker;
  int                   workers;
  WarpCounters*         counters;
  Policy                policy;
  uint32_t              rng;
  int                   backoff_ns;
  int                   sched_iter;
  int                   phase;
  long long             phase_t0;
  long long             start;

  __device__ Worker(KernelArgs<Task, Cfg> a, int w)
      : args(a),
        term{a.spawned, a.executed, a.run, a.worker_count},
        mine(&a.states[w], a.bodies + size_t(w) * Ring::capacity),
        worker(w),
        workers(a.worker_count),
        counters(&a.out[w]),
        policy(a.policy),
        rng(0x9E3779B9u * (w + 1)),
        backoff_ns(a.policy.backoff_min_ns),
        sched_iter(0),
        phase(PHASE_IDLE),
        phase_t0(warp::clock()),
        start(phase_t0) {}

  __device__ Ring ring_of(int victim) const {
    return Ring(&args.states[victim], args.bodies + size_t(victim) * Ring::capacity);
  }

  __device__ uint32_t next_random() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }

  __device__ int enter(int p) {
    int prev = phase;
    if constexpr (Cfg::timing) {
      if (warp::leader()) {
        long long now = warp::clock();
        counters->cycles[phase] += now - phase_t0;
        phase_t0 = now;
      }
    }
    phase = p;
    return prev;
  }

  struct Timed {
    Worker& w;
    int     prev;
    __device__ Timed(Worker& worker, int p) : w(worker), prev(worker.enter(p)) {}
    __device__ ~Timed() { w.enter(prev); }
  };

  __device__ void run() {
    while (!term.done() && !term.aborted()) {
      ++sched_iter;
      if (term.check_due(worker, sched_iter)) return;
      if (warp::leader()) mine.progress();

      Body     body{};
      uint32_t n = 0;
      if (pop_batch(body, n)) {
        execute(body, n);
        release_after_work();
        backoff_ns = policy.backoff_min_ns;
        continue;
      }

      if (try_steal()) {
        backoff_ns = policy.backoff_min_ns;
        continue;
      }

      if (idle()) return;
    }
  }

  __device__ void execute(const Body& body, uint32_t n) {
    Timed t(*this, PHASE_TASK);
    Ctx<Worker> ctx{*this};
    Task::execute(ctx, body, uint32_t(warp::lane()) < n);
    __syncwarp();
    if (warp::leader()) {
      term.add_executed(worker, n);
      counters->chunks++;
    }
  }

  __device__ bool pop_batch(Body& body, uint32_t& n) {
    Timed t(*this, PHASE_POP);
    Range r = warp::lead([&] {
      Range got = mine.pop(C);
      if (got.n == 0 && mine.acquire_half()) {
        counters->acquires++;
        got = mine.pop(C);
      }
      if (got.n > 0) counters->pops++;
      return got;
    });
    if (r.n == 0) return false;
    if (uint32_t(warp::lane()) < r.n) body = warp::load_body(&mine[r.pos + warp::lane()]);
    __syncwarp();
    n = r.n;
    return true;
  }

  __device__ void release_after_work() {
    Timed t(*this, PHASE_QUEUE);
    if (!warp::leader()) return;
    if (mine.release_half()) {
      counters->releases++;
      counters->released_chunks += 1;  // event count; chunk count varies
    }
  }

  __device__ int pick_victim() const {
    const uint32_t r = next_random();
    int v = int(r % uint32_t(workers - 1));
    if (v >= worker) ++v;
    return v;
  }

  __device__ bool try_steal() {
    if (workers <= 1) return false;
    Timed t(*this, PHASE_STEAL_CLAIM);

    const int victim = warp::lead([&] {
      mine.acquire_half();
      return pick_victim();
    });

    const bool can_steal = warp::lead([&] {
      mine.progress();
      Ring vr = ring_of(victim);
      if (!stealable(vr.probe())) return false;
      if (mine.owned_tasks() + Ring::max_steal_chunks * C > Ring::capacity) return false;
      if (mine.used() + Ring::max_steal_chunks * C > Ring::capacity) return false;
      return true;
    });
    if (!can_steal) {
      if (warp::leader() && mine.used() + Ring::max_steal_chunks * C > Ring::capacity)
        counters->pinned_waits++;
      return false;
    }

    struct ClaimResult {
      Ticket ticket;
      bool   guard;
    };

    ClaimResult cr = warp::lead([&] {
      ClaimResult out{{}, false};
      counters->steal_attempts++;
      Ring vr = ring_of(victim);
      uint64_t pre = 0;
      out.ticket = vr.claim(&pre);
      if (steal_guard_tripped(pre)) {
        out.guard = true;
        term.record_error(worker, ERR_STEAL_COUNTER_GUARD);
        out.ticket.valid = false;
      }
      return out;
    });

    if (cr.guard || !cr.ticket.valid) return false;

    Timed tcopy(*this, PHASE_STEAL_COPY);
    const Ticket ticket = cr.ticket;

    Range dest = warp::lead([&] {
      Range d{};
      ReserveStatus rs = mine.incoming_reserve(ticket.n, &d);
      if (rs == ReserveStatus::Full)
        term.record_error(worker, ERR_QUEUE_CAPACITY, ticket.n * C, Ring::capacity - mine.owned_tasks());
      GPUTC_CHECK(rs == ReserveStatus::Ok, "incoming reservation after claim");
      return d;
    });

    Ring vr = ring_of(victim);
    for (uint32_t j = 0; j < ticket.n; ++j) {
      const uint32_t spos = vr.chunk_pos(ticket, j);
      warp::copy_bodies<C>(&mine[dest.pos + j * C], &vr[spos]);
    }
    __syncwarp();

    if (warp::leader()) {
      vr.retire(ticket);
      counters->steal_success++;
      counters->stolen_chunks += ticket.n;
      counters->stolen_tasks += ticket.n * C;
      mine.release_half();
    }
    return true;
  }

  __device__ void spawn(const Body& body, bool pred) {
    unsigned mask = __ballot_sync(warp::FULL, pred);
    if (!mask) return;
    Timed t(*this, PHASE_SPAWN);
    uint32_t count = __popc(mask);
    Range r = reserve(count, count);
    if (r.n == 0) return;
    if (pred) warp::store_body(&mine[r.pos + __popc(mask & warp::lanemask_lt())], body);
    __syncwarp();
    count_spawned(r.n, 1);
    if (warp::leader()) mine.release_half();
  }

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
      if (r.n == 0) return;
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
    if (warp::leader()) mine.release_half();
  }

  __device__ void count_spawned(uint32_t n, uint32_t rounds) {
    if (warp::leader()) {
      term.add_spawned(worker, n);
      counters->spawn_rounds += rounds;
    }
  }

  __device__ Range reserve(uint32_t want, uint32_t min) {
    for (;;) {
      if (term.aborted()) return Range{0, 0};
      ReserveStatus st = ReserveStatus::Retry;
      Range         r{0, 0};
      st = warp::lead([&] {
        ReserveStatus rs = mine.push_reserve(want, min, &r);
        if (rs == ReserveStatus::Ok && mine.used() > counters->deque_peak)
          counters->deque_peak = mine.used();
        return rs;
      });
      if (st == ReserveStatus::Ok) return r;
      if (st == ReserveStatus::Full) {
        warp::lead([&] {
          term.record_error(worker, ERR_QUEUE_CAPACITY, min, Ring::capacity - mine.owned_tasks());
        });
        return Range{0, 0};
      }
      if (warp::leader()) mine.progress();
      __nanosleep(policy.pinned_wait_ns);
    }
  }

  __device__ bool idle() {
    Timed t(*this, PHASE_IDLE);
    if (term.done() || term.aborted()) return true;
    if (warp::leader()) counters->idle_rounds++;
    __nanosleep(backoff_ns);
    backoff_ns = backoff_ns < policy.backoff_max_ns ? min(backoff_ns * 2, policy.backoff_max_ns) : policy.backoff_max_ns;
    if (warp::leader() && worker == 0) term.try_finish();
    return term.done();
  }

  __device__ void finish() {
    enter(PHASE_IDLE);
    if (!warp::leader()) return;
    counters->spawned = DeviceScope::load_relaxed(&args.spawned[worker]);
    counters->tasks   = DeviceScope::load_relaxed(&args.executed[worker]);
    counters->total_cycles = (Cfg::timing ? phase_t0 : warp::clock()) - start;
  }
};

} // namespace gputc
