#pragma once

// Run statistics. Each warp's lane 0 counts events and, when Cfg::timing is
// on, charges every clock cycle of the warp to exactly one Phase. The host
// gets the per-warp counters back and reduces them.

#include <cstdio>
#include <vector>

namespace gputc {

// Where a warp's time goes. Phases nest (a spill runs inside a spawn, which
// runs inside a task); cycles are charged to the innermost one.
enum Phase : int {
  PHASE_TASK,          // Task::execute, excluding the spawn bookkeeping below
  PHASE_SPAWN,         // reserving deque slots and counting spawned tasks
  PHASE_SPILL,         // deque full: moving chunks to the global ring
  PHASE_POP,           // popping the private end, reacquiring the public end
  PHASE_STEAL_SIBLING, // probing and claiming sibling warp deques
  PHASE_GLOBAL,        // taking a chunk from the global ring
  PHASE_RELEASE,       // publishing private chunks for thieves
  PHASE_IDLE,          // nothing found: termination check and backoff
  NUM_PHASES
};

inline const char* phase_name(int p) {
  static const char* const names[NUM_PHASES] = {
    "task", "spawn", "spill", "pop", "steal", "global", "release", "idle"};
  return names[p];
}

// X(name, reduce): every per-warp counter, summed or maxed over warps.
#define GPUTC_COUNTERS(X)                                                        \
  X(tasks, sum)              /* tasks executed */                                \
  X(chunks, sum)             /* chunks executed; tasks / chunks = lanes busy */  \
  X(spawned, sum)            /* tasks spawned */                                 \
  X(spawn_rounds, sum)       /* warp-wide rounds that built spawned tasks */     \
  X(pops, sum)               /* chunks popped from the own private end */        \
  X(reacquires, sum)         /* public ends pulled back by their owner */        \
  X(releases, sum)           /* epochs published */                              \
  X(released_chunks, sum)    /* private chunks made public by releases */        \
  X(deque_peak, max)         /* most tasks held in one warp deque */             \
  X(sibling_probes, sum)     /* probe rounds over sibling steal words */         \
  X(sibling_claims, sum)     /* claims issued against siblings */                \
  X(sibling_steals, sum)     /* ... that returned a chunk */                     \
  X(global_probes, sum)      /* looks at the global ring's chunk count */        \
  X(global_pops, sum)        /* chunks taken from the global ring */             \
  X(spills, sum)             /* deque overflows moved to the global ring */      \
  X(spilled_chunks, sum)     /* chunks those spills moved */                     \
  X(pinned_waits, sum)       /* deque full of chunks thieves were copying */     \
  X(lock_cycles, sum)        /* cycles waiting for the global ring lock */       \
  X(idle_rounds, sum)        /* searches that found nothing */

struct WarpCounters {
#define GPUTC_DECLARE(name, reduce) unsigned long long name;
  GPUTC_COUNTERS(GPUTC_DECLARE)
#undef GPUTC_DECLARE
  unsigned long long cycles[NUM_PHASES];
  unsigned long long total_cycles;  // from the warp's start to its exit
};

struct RunShape {
  int  warps;
  int  chunk;
  int  warp_chunks;
  int  global_chunks;
  bool timing;
};

struct Stats {
  RunShape shape;
  double   seconds;      // kernel time, from CUDA events around the launch
  unsigned long long global_peak;  // most tasks held in the global ring
  std::vector<WarpCounters> per_warp;
  WarpCounters total;    // sums over warps (maxima for peaks)

  void reduce() {
    total = WarpCounters{};
    for (const WarpCounters& w : per_warp) {
#define GPUTC_REDUCE_sum(name) total.name += w.name;
#define GPUTC_REDUCE_max(name) total.name = total.name > w.name ? total.name : w.name;
#define GPUTC_REDUCE(name, reduce) GPUTC_REDUCE_##reduce(name)
      GPUTC_COUNTERS(GPUTC_REDUCE)
#undef GPUTC_REDUCE
#undef GPUTC_REDUCE_max
#undef GPUTC_REDUCE_sum
      for (int p = 0; p < NUM_PHASES; ++p) total.cycles[p] += w.cycles[p];
      total.total_cycles += w.total_cycles;
    }
  }

  bool conserved() const { return total.spawned == total.tasks; }
  double tasks_per_second() const { return seconds > 0 ? double(total.tasks) / seconds : 0; }
  // Average fraction of a chunk's lanes that had a task.
  double lane_fill() const { return total.chunks ? double(total.tasks) / (double(total.chunks) * shape.chunk) : 0; }
  // Average fraction of lanes that built a child in each spawn round.
  double spawn_fill() const {
    return total.spawn_rounds ? double(total.spawned) / (double(total.spawn_rounds) * shape.chunk) : 0;
  }
  double phase_fraction(const WarpCounters& w, int p) const {
    return w.total_cycles ? double(w.cycles[p]) / double(w.total_cycles) : 0;
  }

  void print(std::FILE* f = stdout, bool per_warp_table = false) const {
    const WarpCounters& t = total;
    auto pct = [](double x) { return 100.0 * x; };
    auto ratio = [](unsigned long long a, unsigned long long b) { return b ? double(a) / double(b) : 0.0; };

    std::fprintf(f, "gputc: %llu tasks in %.4f s = %.2f M tasks/s  (1 block x %d warps, deque %d chunks, global ring %d chunks)\n",
                 t.tasks, seconds, tasks_per_second() / 1e6, shape.warps, shape.warp_chunks, shape.global_chunks);
    std::fprintf(f, "  conservation  spawned %llu, executed %llu: %s\n", t.spawned, t.tasks,
                 conserved() ? "ok" : "MISMATCH");
    std::fprintf(f, "  lanes         executing: %.1f%% of lanes hold a task (%.1f per chunk); spawning: %.1f%% of lanes build a child\n",
                 pct(lane_fill()), ratio(t.tasks, t.chunks), pct(spawn_fill()));
    if (shape.timing && t.total_cycles) {
      std::fprintf(f, "  warp time    ");
      for (int p = 0; p < NUM_PHASES; ++p) std::fprintf(f, " %s %.1f%%", phase_name(p), pct(phase_fraction(t, p)));
      std::fprintf(f, "\n");
      std::fprintf(f, "                scheduler overhead %.1f%% (everything but task and idle)\n",
                   pct(1.0 - phase_fraction(t, PHASE_TASK) - phase_fraction(t, PHASE_IDLE)));
      std::fprintf(f, "  cycles/chunk ");
      for (int p = 0; p < NUM_PHASES; ++p) std::fprintf(f, " %s %.0f", phase_name(p), ratio(t.cycles[p], t.chunks));
      std::fprintf(f, "  (warp cycles per executed chunk)\n");
    }
    std::fprintf(f, "  chunk source  own deque %.1f%%, siblings %.1f%%, global ring %.1f%% of %llu chunks\n",
                 pct(ratio(t.pops, t.chunks)), pct(ratio(t.sibling_steals, t.chunks)),
                 pct(ratio(t.global_pops, t.chunks)), t.chunks);
    std::fprintf(f, "  steals        %llu of %llu claims succeeded (%llu probe rounds found a victim %.1f%% of the time)\n",
                 t.sibling_steals, t.sibling_claims, t.sibling_probes, pct(ratio(t.sibling_claims, t.sibling_probes)));
    std::fprintf(f, "  deque         releases %llu (%llu chunks), reacquires %llu, peak %.1f of %d chunks\n",
                 t.releases, t.released_chunks, t.reacquires, double(t.deque_peak) / shape.chunk, shape.warp_chunks);
    std::fprintf(f, "  global ring   spills %llu (%llu chunks), pops %llu, peak %.1f of %d chunks, pinned waits %llu\n",
                 t.spills, t.spilled_chunks, t.global_pops, double(global_peak) / shape.chunk, shape.global_chunks,
                 t.pinned_waits);
    if (shape.timing && t.total_cycles)
      std::fprintf(f, "                lock wait %.2f%% of warp time\n", pct(ratio(t.lock_cycles, t.total_cycles)));
    std::fprintf(f, "  idle          %llu empty searches\n", t.idle_rounds);

    if (per_warp_table) print_per_warp(f);
  }

  void print_per_warp(std::FILE* f = stdout) const {
    std::fprintf(f, "  warp     tasks  lanes%%");
    if (shape.timing)
      for (int p = 0; p < NUM_PHASES; ++p) std::fprintf(f, " %9s", phase_name(p));
    std::fprintf(f, "   steals  gl-pops  rel-ch  spill-ch\n");
    for (size_t i = 0; i < per_warp.size(); ++i) {
      const WarpCounters& w = per_warp[i];
      std::fprintf(f, "  %4zu %9llu  %5.1f", i, w.tasks,
                   w.chunks ? 100.0 * double(w.tasks) / (double(w.chunks) * shape.chunk) : 0.0);
      if (shape.timing)
        for (int p = 0; p < NUM_PHASES; ++p) std::fprintf(f, " %8.1f%%", 100.0 * phase_fraction(w, p));
      std::fprintf(f, " %8llu %8llu %7llu %9llu\n", w.sibling_steals, w.global_pops, w.released_chunks, w.spilled_chunks);
    }
  }

  // One line of key=value pairs for scripts collecting many runs.
  void print_kv(std::FILE* f = stdout) const {
    std::fprintf(f, "gputc_kv seconds=%.6f tasks_per_s=%.0f lane_fill=%.4f spawn_fill=%.4f warps=%d warp_chunks=%d global_chunks=%d global_peak=%llu",
                 seconds, tasks_per_second(), lane_fill(), spawn_fill(), shape.warps, shape.warp_chunks,
                 shape.global_chunks, global_peak);
#define GPUTC_PRINT(name, reduce) std::fprintf(f, " " #name "=%llu", total.name);
    GPUTC_COUNTERS(GPUTC_PRINT)
#undef GPUTC_PRINT
    if (shape.timing)
      for (int p = 0; p < NUM_PHASES; ++p) std::fprintf(f, " time_%s=%.4f", phase_name(p), phase_fraction(total, p));
    std::fprintf(f, "\n");
  }
};

} // namespace gputc
