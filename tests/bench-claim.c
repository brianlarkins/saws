/*
 * bench-claim.c -- E3.2 (H1): claim latency on one target with N concurrent node-mate initiators,
 * the two-door queue's near door next to SAWS's on-node claim.
 *
 *   srun -N1 --ntasks-per-node=48 --mpi=pmix --cpu-bind=cores ./bench-claim [-t tasks] [-d secs] [-p 0|1] [-q]
 *
 * PE 0 owns one queue of each type and keeps it stocked: whenever its public part is empty it
 * tops its private part up to 2T tasks and releases T (SAWS's rule).  On the two-door queue rho is
 * 0.001, so all but one released task sit behind the near door; refill and re-split are off.  When
 * the near block is exhausted the owner drains: it pops its private tasks, and the dry pop collapses
 * the near region and reacquires the one far task, which empties the queue for the next release.
 * PEs 1..N claim from PE 0 in a loop for d seconds (SAWS: its claim; two-door: the near door only),
 * timing every attempt with the TSC; stolen tasks are discarded.  Peeks (-p, default 1) precede
 * claims on both queues, as in the laws policies.
 * With -q the owner calls shmem_quiet() after each release, so its non-blocking loopback atomic_set of the
 * far word (SAWS's steal_val) is known to have landed; without it the owner, which makes no other SHMEM
 * calls while it waits, may leave the set pending.
 * Prints one line per (queue, N): successful claims and mean us per success, failed attempts and
 * mean us per failure, tasks per success, and owner rounds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <tc.h>
#include <saws_shrb.h>
#include <laws_shrb.h>

typedef struct { int seq; char pad[44]; } btask_t;   // 64-byte queue elements with the task header

static long *stop;         // symmetric flag on PE 0
static int quiet_after = 0;
static double hz;          // TSC ticks per microsecond

static void stock(void *b, int qt, void *buf, int T, int esz) {   // owner: top up to 2T private, release T
  if (qt == GtcQueueSAWS) {
    saws_shrb_t *rb = b;
    int need = 2 * T - rb->nlocal;
    if (need > 0) saws_shrb_push_n_head(rb, rb->procid, buf, need);
    saws_shrb_release(rb);
  } else {
    laws_shrb_t *rb = b;
    int need = 2 * T - rb->nlocal;
    if (need > 0) laws_shrb_push_n_head(rb, rb->procid, buf, need);
    laws_shrb_release(rb);
  }
  if (quiet_after) shmem_quiet();
  UNUSED(esz);
}

static long owner(void *b, int qt, void *buf, int T, int esz, double secs) {
  uint64_t end = gtc_get_tsctime() + (uint64_t)(secs * hz * 1e6);
  void *one = malloc(esz);
  long rounds = 0;

  while (gtc_get_tsctime() < end) {
    if (qt == GtcQueueSAWS) {
      saws_shrb_t *rb = b;
      saws_shrb_reclaim_space(rb);
      if (saws_shrb_shared_size(rb) == 0) { stock(b, qt, buf, T, esz); rounds++; }
    } else {
      laws_shrb_t *rb = b;
      laws_shrb_reclaim_space(rb);
      if (laws_shrb_public_size(rb) == 0) { stock(b, qt, buf, T, esz); rounds++; }
      else if (laws_shrb_near_unclaimed(rb) == 0) {
        while (laws_shrb_pop_head(rb, rb->procid, one)) ;   // private, then collapse + far reacquire
        laws_shrb_reclaim_space(rb);
      }
    }
  }
  __atomic_store_n(stop, 1, __ATOMIC_RELEASE);
  free(one);
  return rounds;
}

int main(int argc, char **argv) {
  int T = 1024, peek = 1, arg;
  double secs = 2.0;
  int ns[] = { 1, 8, 16, 32, 47 };

  gtc_init();
  while ((arg = getopt(argc, argv, "t:d:p:q")) != -1) {
    switch (arg) {
      case 't': T = atoi(optarg); break;
      case 'd': secs = atof(optarg); break;
      case 'p': peek = atoi(optarg); break;
      case 'q': quiet_after = 1; break;
    }
  }
  int me = _c->rank, npes = _c->size;
  hz = _c->tsc_cpu_hz;   // MHz = ticks per us

  gtc_ldbal_cfg_t cfg;
  gtc_ldbal_cfg_init(&cfg);
  cfg.near_peek    = peek;
  cfg.laws_rho_pm  = 1;      // rho = 0.001: one task far, the rest near
  cfg.laws_refill  = 0;
  cfg.laws_resplit = 0;
  gtc_t gs = gtc_create(sizeof(btask_t), 10, 8 * T, &cfg, GtcQueueSAWS);
  gtc_t gl = gtc_create(sizeof(btask_t), 10, 8 * T, &cfg, GtcQueueLAWS);
  tc_t *ts = gtc_lookup(gs), *tl = gtc_lookup(gl);
  int esz = sizeof(task_t) + sizeof(btask_t);

  stop = shmem_calloc(1, sizeof(long));
  long *stopp = shmem_ptr(stop, 0);
  void *buf  = calloc(2 * T, esz);          // owner's stock (contents do not matter)
  void *sbuf = malloc((size_t)8 * T * esz); // initiator's steal buffer
  uint64_t *st = shmem_calloc(5, sizeof(uint64_t)), *sum = shmem_calloc(5, sizeof(uint64_t));

  if (me == 0)
    printf("bench-claim: %d PEs on %d node(s), block T = %d tasks of %d bytes, %.1f s per point, peeks %s, "
           "near door %s, shmem_ptr %s, quiet after release %s\n", npes, _c->loc.nnodes, T, esz, secs, peek ? "on" : "off",
           ((laws_shrb_t *)tl->shared_rb)->near_on ? "on" : "OFF", stopp ? "yes" : "no", quiet_after ? "yes" : "no");

  for (int qi = 0; qi < 2; qi++) {
    int qt = qi == 0 ? GtcQueueSAWS : GtcQueueLAWS;
    void *rb = qi == 0 ? ts->shared_rb : tl->shared_rb;
    for (unsigned k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
      int N = ns[k];
      if (N > npes - 1 || !gtc_is_near(N)) continue;   // initiators must be PE 0's node-mates
      if (qt == GtcQueueSAWS) saws_shrb_reset(rb); else laws_shrb_reset(rb);
      *stop = 0;
      memset(st, 0, 5 * sizeof(uint64_t));
      shmem_barrier_all();

      long rounds = 0;
      if (me == 0) {
        stock(rb, qt, buf, T, esz);
        shmem_barrier_all();                       // initiators start once the first block is out
        rounds = owner(rb, qt, buf, T, esz, secs) + 1;
      } else {
        shmem_barrier_all();
        if (me <= N) {
          while (!__atomic_load_n(stopp, __ATOMIC_ACQUIRE)) {
            uint64_t t0 = gtc_get_tsctime();
            int got = (qt == GtcQueueSAWS) ? saws_shrb_pop_n_tail(rb, 0, 0, sbuf, STEAL_HALF)
                                           : laws_shrb_claim(rb, 0, LAWS_DOOR_NEAR, sbuf);
            uint64_t dt = gtc_get_tsctime() - t0;
            if (got > 0) { st[0]++; st[1] += dt; st[4] += got; }
            else         { st[2]++; st[3] += dt; }
          }
        }
      }
      shmem_barrier_all();
      shmem_uint64_sum_reduce(SHMEM_TEAM_WORLD, sum, st, 5);
      if (me == 0)
        printf("%-9s N=%2d  ok %8lu  %7.3f us/ok  fail %9lu  %7.3f us/fail  %6.2f tasks/ok  %6ld rounds  %8.0f ok/s\n",
               qt == GtcQueueSAWS ? "SAWS" : "near-door", N,
               sum[0], sum[0] ? sum[1] / hz / sum[0] : 0.0, sum[2], sum[2] ? sum[3] / hz / sum[2] : 0.0,
               sum[0] ? (double)sum[4] / sum[0] : 0.0, rounds, sum[0] / secs);
      // drain: let in-flight completions land, then empty the queues for the next point
      shmem_barrier_all();
    }
  }

  shmem_barrier_all();
  gtc_destroy(gl);
  gtc_destroy(gs);
  gtc_fini();
  return 0;
}
