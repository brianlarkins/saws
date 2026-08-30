/** gpubpc.cu -- Bouncing Producer-Consumer microbenchmark on libgputc
 *
 *  GPU port of examples/bpc/bpc.c.
 *  (c) 2026 D. Brian Larkins
 *  Original CPU version by James Dinan, August 2008.
 *
 *  BPC is the right first application for this system for a reason that has
 *  nothing to do with its original purpose.  It has exactly two task classes
 *  with a tunable cost ratio (-p and -c), so it is a direct knob on the
 *  load-versus-coherence tension that a GPU work stealer has to resolve and a
 *  CPU one does not.  Set the ratio to 1:1 and coherence is free; set it to
 *  1:10 and a warp that steals a mixed batch wastes most of its lanes waiting
 *  on the slow class.
 *
 *  Structure of the computation, unchanged from the CPU version:
 *
 *      producer(level) -> nchildren x consumer(level+1)
 *                      -> 1 x producer(level+1)      until level == maxdepth
 *
 *  so the producers form a serial chain that continuously injects a burst of
 *  independent consumers.  The chain is the bottleneck and the consumers are
 *  the work to be balanced.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include <cuda_runtime.h>
#include <nvshmem.h>
#include <nvshmemx.h>

#include "gputc.h"
#include "gputc_types.cuh"
#include "gpu_shrb.cuh"
#include "gpu_task.cuh"


/*==================== TASK PAYLOAD ====================*/

/** Mirrors pctask_t.  The CLO keys are gone: on the GPU the counters are
 *  device globals reached with atomics rather than per-PE replicated objects
 *  looked up by key, which removes an indirection from the task body. */
typedef struct {
  int parent_pe;
  int level;
  int index;
} pctask_t;


/*==================== PARAMETERS ====================*/
/*
 *  __constant__ rather than kernel arguments, because the task bodies are
 *  reached through a function pointer and cannot take extra parameters.
 */
__constant__ int    d_maxdepth;
__constant__ int    d_nchildren;
__constant__ int    d_bouncing;
__constant__ long long d_producer_cycles;
__constant__ long long d_consumer_cycles;

/* Result counters. */
__device__ unsigned long long d_nproducers = 0;
__device__ unsigned long long d_nconsumers = 0;
__device__ unsigned long long d_ntasks     = 0;

/* Host-side mirrors. */
static int    maxdepth          = 512;
static int    nchildren         = 10;
static int    initial_producers = 64;
static int    bouncing          = 0;
static int    verbose           = 0;
static double producer_work_units = 1.0;
static double consumer_work_units = 10.0;

static const double work_time = 0.001;   /* 1 ms, as in the CPU version */

/** Assumed clock for converting work units to cycles.  The CPU version
 *  calibrates a busy_wait; here we just use the reported SM clock. */
static double gpu_clock_hz = 1.4e9;


/*==================== DEVICE BUSY WAIT ====================*/

/** Stand-in for the CPU version's nanosleep()/busy_wait().
 *
 *  A real port would not spin -- burning an SM to simulate work is exactly
 *  backwards on a GPU -- but the benchmark's entire purpose is to impose a
 *  known, tunable cost per task, so a spin is the honest translation.
 *
 *  Note this is a *per-lane* spin, so a warp's cost is the max over its lanes.
 *  With a uniform class across the warp that max equals the common case, which
 *  is precisely the property class-segregated queues are meant to preserve.
 */
__device__ __forceinline__ void gpu_busy_wait(long long cycles)
{
  long long start = clock64();
  while ((clock64() - start) < cycles) { /* spin */ }
}


/*==================== TASK BODIES ====================*/

/* Class ids, filled in by main() and pushed to the device. */
__constant__ int d_producer_class;
__constant__ int d_consumer_class;


/** Consumer: pure work, no children.  The common case by a factor of
 *  nchildren, and the class that actually needs balancing. */
GPUTC_TASK(consumer_task)
{
  pctask_t *tt = (pctask_t *)body;
  (void)tt;

  gpu_busy_wait(d_consumer_cycles);

  atomicAdd(&d_nconsumers, 1ULL);
  atomicAdd(&d_ntasks,     1ULL);
}


/** Producer: spawns a burst of consumers plus its own successor.
 *
 *  Every spawn is a gputc_dev_add() into this block's own deque for the target
 *  class, which is the direct analogue of gtc_add(gtc, task, me) in the CPU
 *  version.  Work migrates only by being stolen, so the spawn path stays free
 *  of remote traffic and the burst becomes visible to thieves as soon as the
 *  block's next maintenance pass releases it.
 */
GPUTC_TASK(producer_task)
{
  pctask_t *tt = (pctask_t *)body;

  if (tt->level < d_maxdepth) {
    pctask_t child;
    child.parent_pe = ctx->my_pe;
    child.level     = tt->level + 1;

    /* Bouncing mode spawns the successor first, so the producer chain stays
     * ahead of the consumer burst and is more likely to be stolen away --
     * which is the whole point of the mode. */
    if (d_bouncing) {
      child.index = tt->index;
      gputc_dev_add(ctx, (unsigned int)d_producer_class, &child);
    }

    for (int i = 0; i < d_nchildren; i++) {
      child.index = tt->index * d_nchildren + i;
      if (!gputc_dev_add(ctx, (unsigned int)d_consumer_class, &child)) {
        /* Queue full.  The prototype has no overflow path, so run the child
         * inline rather than lose it; this preserves the task count at the
         * cost of the load balance.  See libgputc/README.md, "Known gaps". */
        gpu_busy_wait(d_consumer_cycles);
        atomicAdd(&d_nconsumers, 1ULL);
        atomicAdd(&d_ntasks,     1ULL);
      }
    }

    if (!d_bouncing) {
      child.index = tt->index;
      gputc_dev_add(ctx, (unsigned int)d_producer_class, &child);
    }
  }

  gpu_busy_wait(d_producer_cycles);

  atomicAdd(&d_nproducers, 1ULL);
  atomicAdd(&d_ntasks,     1ULL);
}


/*==================== ARGUMENT HANDLING ====================*/

static void process_args(int argc, char **argv, int me)
{
  int arg;
  char *endptr;

  while ((arg = getopt(argc, argv, "d:n:i:p:c:bvh")) != -1) {
    switch (arg) {
      case 'd': maxdepth  = strtol(optarg, &endptr, 10); break;
      case 'n': nchildren = strtol(optarg, &endptr, 10); break;
      case 'i': initial_producers = strtol(optarg, &endptr, 10); break;
      case 'p': producer_work_units = strtod(optarg, &endptr); break;
      case 'c': consumer_work_units = strtod(optarg, &endptr); break;
      case 'b': bouncing = 1; break;
      case 'v': verbose  = 1; break;
      case 'h':
        if (me == 0) {
          printf("GPU Producer-Consumer Microbenchmark (libgputc)\n");
          printf("  Usage: %s [args]\n\n", basename(argv[0]));
          printf("  -d int  %5d  Max depth\n", maxdepth);
          printf("  -n int  %5d  Children per producer\n", nchildren);
          printf("  -i int  %5d  Initial producers\n", initial_producers);
          printf("  -p dbl  %5.2f  Producer work (units of %.2f ms)\n",
                 producer_work_units, work_time);
          printf("  -c dbl  %5.2f  Consumer work (units of %.2f ms)\n",
                 consumer_work_units, work_time);
          printf("  -b             Bouncing mode\n");
          printf("  -v             Verbose\n");
        }
        exit(0);
      default:
        if (me == 0) printf("Try '-h' for help.\n");
        exit(1);
    }
  }
}


/*==================== MAIN ====================*/

int main(int argc, char **argv)
{
  gputc_init();

  int me    = gputc_my_pe();
  int nproc = gputc_n_pes();

  process_args(argc, argv, me);

  /* Convert work units to a spin count using the device's clock. */
  {
    int dev; cudaGetDevice(&dev);
    cudaDeviceProp prop; cudaGetDeviceProperties(&prop, dev);
    gpu_clock_hz = prop.clockRate * 1000.0;
  }

  long long pcycles = (long long)(producer_work_units * work_time * gpu_clock_hz);
  long long ccycles = (long long)(consumer_work_units * work_time * gpu_clock_hz);

  /* Expected totals, same formulas as the CPU version.  initial_producers is
   * scaled up relative to the CPU default because a single producer chain is
   * serial and would leave a GPU almost entirely idle -- the parallelism has
   * to come from having many chains in flight. */
  long long expected_nproducers = (long long)initial_producers * (maxdepth + 1);
  long long expected_nconsumers = (long long)initial_producers * ((long long)maxdepth * nchildren);
  long long expected_ntasks     = expected_nproducers + expected_nconsumers;

  double ideal_walltime =
      (expected_nproducers * work_time * producer_work_units +
       expected_nconsumers * work_time * consumer_work_units) / nproc;

  if (me == 0) {
    printf("GPU Producer-Consumer uBench, %d PEs\n", nproc);
    printf("-----------------------------------------------------------------\n");
    printf("depth=%d children=%d initial=%d  producers=%lld consumers=%lld\n",
           maxdepth, nchildren, initial_producers,
           expected_nproducers, expected_nconsumers);
    printf("work unit=%.2f ms  producer=%.2f units  consumer=%.2f units\n",
           work_time * 1000.0, producer_work_units, consumer_work_units);
    printf("ideal walltime=%.4f s\n\n", ideal_walltime);
  }

  /* ---- build the collection ---- */
  gputc_cfg_t cfg;
  gputc_cfg_default(&cfg);
  cfg.ring_size       = 16384;
  cfg.coherence_aware = 1;
  cfg.verbose         = verbose;

  gputc_t gtc = gputc_create(sizeof(pctask_t), &cfg, GputcQueueSAWS);
  if (gtc < 0) { fprintf(stderr, "gputc_create failed\n"); return 1; }

  /* ---- register the two classes ----
   *
   * Order must match on every PE so that class ids agree, exactly as with
   * gtc_task_class_register() in the CPU version. */
  gputc_class_t pcls = gputc_task_class_register(gtc, sizeof(pctask_t),
                                                 GPUTC_CLASS_FN(producer_task));
  gputc_class_t ccls = gputc_task_class_register(gtc, sizeof(pctask_t),
                                                 GPUTC_CLASS_FN(consumer_task));

  cudaMemcpyToSymbol(d_producer_class,  &pcls,      sizeof(pcls));
  cudaMemcpyToSymbol(d_consumer_class,  &ccls,      sizeof(ccls));
  cudaMemcpyToSymbol(d_maxdepth,        &maxdepth,  sizeof(maxdepth));
  cudaMemcpyToSymbol(d_nchildren,       &nchildren, sizeof(nchildren));
  cudaMemcpyToSymbol(d_bouncing,        &bouncing,  sizeof(bouncing));
  cudaMemcpyToSymbol(d_producer_cycles, &pcycles,   sizeof(pcycles));
  cudaMemcpyToSymbol(d_consumer_cycles, &ccycles,   sizeof(ccycles));

  /* ---- seed ----
   *
   * PE 0 injects every root producer, spread across its blocks so the initial
   * dispersion phase has something to steal from more than one queue.  The CPU
   * version has the same FIXME about everything landing on one process; here
   * the round-robin across blocks at least gives the intra-device tier
   * somewhere to start.
   */
  if (me == 0) {
    for (int i = 0; i < initial_producers; i++) {
      pctask_t root = { me, 0, i };
      gputc_seed(gtc, pcls, &root, i % cfg.nblocks);
    }
  }

  if (me == 0)
    printf("%sProducer-Consumer test starting...\n\n",
           bouncing ? "Bouncing " : "");

  /* ---- run ---- */
  gputc_process(gtc);

  /* ---- collect ---- */
  unsigned long long h_ntasks = 0, h_nprod = 0, h_ncons = 0;
  cudaMemcpyFromSymbol(&h_ntasks, d_ntasks,     sizeof(h_ntasks));
  cudaMemcpyFromSymbol(&h_nprod,  d_nproducers, sizeof(h_nprod));
  cudaMemcpyFromSymbol(&h_ncons,  d_nconsumers, sizeof(h_ncons));

  unsigned long long *src = (unsigned long long *)nvshmem_malloc(3 * sizeof(unsigned long long));
  unsigned long long *dst = (unsigned long long *)nvshmem_malloc(3 * sizeof(unsigned long long));
  unsigned long long local[3] = { h_ntasks, h_nprod, h_ncons };

  cudaMemcpy(src, local, sizeof(local), cudaMemcpyHostToDevice);
  nvshmem_ulonglong_sum_reduce(NVSHMEM_TEAM_WORLD, dst, src, 3);
  nvshmem_barrier_all();

  unsigned long long total[3];
  cudaMemcpy(total, dst, sizeof(total), cudaMemcpyDeviceToHost);

  if (me == 0) {
    gputc_stats_t st;
    gputc_get_stats(gtc, &st);
    double t = st.walltime > 0.0 ? st.walltime : 1e-9;

    printf("\n");
    printf("Total tasks    = %8llu, expected = %8lld: %s\n",
           total[0], expected_ntasks,
           ((long long)total[0] == expected_ntasks) ? "SUCCESS" : "FAILURE");
    printf("Producer tasks = %8llu, expected = %8lld: %s\n",
           total[1], expected_nproducers,
           ((long long)total[1] == expected_nproducers) ? "SUCCESS" : "FAILURE");
    printf("Consumer tasks = %8llu, expected = %8lld: %s\n",
           total[2], expected_nconsumers,
           ((long long)total[2] == expected_nconsumers) ? "SUCCESS" : "FAILURE");
    printf("\n");
    printf("Actual walltime = %.4f s, %.0f tasks/sec\n", t, total[0] / t);
    printf(" Ideal walltime = %.4f s, %.0f tasks/sec\n",
           ideal_walltime, total[0] / ideal_walltime);
    printf("     Efficiency = %.1f%%\n\n", 100.0 * ideal_walltime / t);
  }

  nvshmem_barrier_all();
  gputc_print_stats(gtc);
  nvshmem_barrier_all();

  nvshmem_free(src);
  nvshmem_free(dst);
  gputc_destroy(gtc);
  gputc_fini();
  return 0;
}
