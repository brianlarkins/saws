#pragma once

#include <cstdio>
#include <vector>

namespace gputc {

enum Phase : int {
  PHASE_TASK,
  PHASE_SPAWN,
  PHASE_POP,
  PHASE_STEAL_CLAIM,
  PHASE_STEAL_COPY,
  PHASE_QUEUE,
  PHASE_IDLE,
  NUM_PHASES
};

inline const char* phase_name(int p) {
  static const char* const names[NUM_PHASES] = {
    "task", "spawn", "pop", "steal_claim", "steal_copy", "queue", "idle"};
  return names[p];
}

#define GPUTC_COUNTERS(X)                                                        \
  X(tasks, sum)                                                                  \
  X(chunks, sum)                                                                 \
  X(spawned, sum)                                                                \
  X(spawn_rounds, sum)                                                           \
  X(pops, sum)                                                                   \
  X(acquires, sum)                                                               \
  X(releases, sum)                                                               \
  X(released_chunks, sum)                                                        \
  X(deque_peak, max)                                                             \
  X(steal_attempts, sum)                                                         \
  X(steal_success, sum)                                                          \
  X(stolen_chunks, sum)                                                          \
  X(stolen_tasks, sum)                                                           \
  X(pinned_waits, sum)                                                           \
  X(idle_rounds, sum)

struct WarpCounters {
#define GPUTC_DECLARE(name, reduce) unsigned long long name;
  GPUTC_COUNTERS(GPUTC_DECLARE)
#undef GPUTC_DECLARE
  unsigned long long cycles[NUM_PHASES];
  unsigned long long total_cycles;
};

struct RunShape {
  int  blocks;
  int  warps_per_block;
  int  workers;
  int  chunk;
  int  deque_chunks;
  bool timing;
  size_t deque_bytes;
};

struct Stats {
  RunShape shape;
  double   seconds;
  int      error_code;
  int      error_worker;
  unsigned long long global_spawned;
  unsigned long long global_executed;
  unsigned int max_blocks;
  unsigned int registers;
  unsigned int static_smem;
  unsigned int dynamic_smem;
  std::vector<WarpCounters> per_warp;
  WarpCounters total;

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

  bool conserved() const {
    return error_code == 0 && global_spawned == global_executed && total.spawned == total.tasks;
  }

  double tasks_per_second() const { return seconds > 0 ? double(total.tasks) / seconds : 0; }

  double lane_fill() const {
    return total.chunks ? double(total.tasks) / (double(total.chunks) * shape.chunk) : 0;
  }

  double phase_fraction(const WarpCounters& w, int p) const {
    return w.total_cycles ? double(w.cycles[p]) / double(w.total_cycles) : 0;
  }

  void print(std::FILE* f = stdout, bool per_warp_table = false) const {
    const WarpCounters& t = total;
    std::fprintf(f,
      "gputc: %llu tasks in %.4f s = %.2f M tasks/s  (%d blocks x %d warps = %d workers, deque %d chunks, %.2f MiB/worker)\n",
      t.tasks, seconds, tasks_per_second() / 1e6, shape.blocks, shape.warps_per_block, shape.workers,
      shape.deque_chunks, double(shape.deque_bytes) / (1024.0 * 1024.0));
    std::fprintf(f, "  status        error=%d worker=%d  spawned=%llu executed=%llu %s\n",
                 error_code, error_worker, global_spawned, global_executed,
                 conserved() ? "conserved" : "MISMATCH");
    std::fprintf(f, "  steals        %llu ok of %llu attempts (%llu chunks, %llu tasks)\n",
                 t.steal_success, t.steal_attempts, t.stolen_chunks, t.stolen_tasks);
    std::fprintf(f, "  queue         releases %llu (%llu chunks), acquires %llu, peak %llu tasks\n",
                 t.releases, t.released_chunks, t.acquires, t.deque_peak);
    std::fprintf(f, "  idle          %llu rounds, %llu pinned waits\n", t.idle_rounds, t.pinned_waits);
    if (max_blocks) std::fprintf(f, "  launch        max cooperative blocks=%u  kernel regs=%u static_smem=%u\n",
                                 max_blocks, registers, static_smem);
    if (shape.timing && t.total_cycles) {
      std::fprintf(f, "  warp time    ");
      for (int p = 0; p < NUM_PHASES; ++p)
        std::fprintf(f, " %s %.1f%%", phase_name(p), 100.0 * phase_fraction(t, p));
      std::fprintf(f, "\n");
    }
    if (per_warp_table) print_per_warp(f);
  }

  void print_per_warp(std::FILE* f = stdout) const {
    std::fprintf(f, "  worker   tasks  steals  rel-ch  peak\n");
    for (size_t i = 0; i < per_warp.size(); ++i) {
      const WarpCounters& w = per_warp[i];
      std::fprintf(f, "  %4zu %9llu %7llu %7llu %7llu\n", i, w.tasks, w.steal_success,
                   w.released_chunks, w.deque_peak);
    }
  }

  void print_kv(std::FILE* f = stdout) const {
    std::fprintf(f,
      "gputc_kv seconds=%.6f tasks_per_s=%.0f blocks=%d warps=%d workers=%d deque_chunks=%d deque_bytes=%zu error=%d",
      seconds, tasks_per_second(), shape.blocks, shape.warps_per_block, shape.workers, shape.deque_chunks,
      shape.deque_bytes, error_code);
#define GPUTC_PRINT(name, reduce) std::fprintf(f, " " #name "=%llu", total.name);
    GPUTC_COUNTERS(GPUTC_PRINT)
#undef GPUTC_PRINT
    std::fprintf(f, "\n");
  }
};

} // namespace gputc
