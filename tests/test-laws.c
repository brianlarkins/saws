/*
 * test-laws.c -- checks the LAWS node work bitmap (owner-only bits) and the laws policy.
 *
 *   srun --mpi=pmix -N <nodes> --ntasks-per-node=<ppn> ./test-laws [-L]
 *
 * -L uses the two-door queue: then both door bitmaps are checked (B_far in word 0, which
 * other nodes read; B_near in word 1, when the near door is on: rho < 1 and node-mates).
 *
 * 1. every PE adds tasks and calls progress: every owner's bit must be set, both in
 *    my node's bitmap (shmem_ptr load) and in another node's (remote get);
 * 2. gtc_process under the laws policy with peeks: every task runs exactly once;
 * 3. afterwards, with empty queues and enough progress calls for the lazy clear
 *    (laws_poll), every bit is clear.
 * Prints "LAWS test: SUCCESS" or "LAWS test: FAILURE (<n> errors)" on rank 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <unistd.h>
#include <tc.h>
#include <laws_shrb.h>

#define NTASKS 200

static int errors = 0;
static long executed = 0;

#define CHECK(cond, ...) do { if (!(cond)) { errors++; if (errors <= 5) { \
      printf("rank %d: FAILED: ", _c->rank); printf(__VA_ARGS__); printf("\n"); } } } while (0)

typedef struct { int origin; } ltask_t;

static void task_fcn(gtc_t gtc, task_t *task) {
  UNUSED(gtc); UNUSED(task);
  for (volatile int i = 0; i < 20000; i++) ;   // a little work so steals happen
  executed++;
}

static uint64_t full_mask(int n) {
  return (gtc_node_size(n) >= 64) ? ~0ULL : ((1ULL << gtc_node_size(n)) - 1);
}

int main(int argc, char **argv) {
  gtc_qtype_t qtype = GtcQueueSAWS;
  int arg;
  gtc_init();
  while ((arg = getopt(argc, argv, "L")) != -1)
    if (arg == 'L') qtype = GtcQueueLAWS;

  gtc_locality_t *L = &_c->loc;
  int me = _c->rank, npes = _c->size;

  gtc_ldbal_cfg_t cfg;
  gtc_ldbal_cfg_init(&cfg);
  cfg.target_selection = TARGET_LAWS;
  cfg.near_peek = 1;
  gtc_t gtc = gtc_create(sizeof(ltask_t), 10, 100000, &cfg, qtype);
  tc_t *tc = gtc_lookup(gtc);
  task_class_t tclass = gtc_task_class_register(sizeof(ltask_t), task_fcn);

  if (tc->ldbal_cfg.target_selection != TARGET_LAWS) {
    if (me == 0) printf("LAWS test: SKIPPED (laws policy unavailable: needs SAWS and shmem_ptr)\n");
    gtc_destroy(gtc);
    gtc_fini();
    return 0;
  }

  // 1. publish work: every owner's bit must be set
  for (int i = 0; i < NTASKS; i++) {
    task_t *t = gtc_task_create(tclass);
    ((ltask_t *)gtc_task_body(t))->origin = me;
    gtc_add(gtc, t, me);
    gtc_task_destroy(t);
  }
  gtc_progress(gtc);   // releases half of the local work and sets my bit
  shmem_barrier_all();
  CHECK(tc->bit_set == 1, "own bit not set after release");
  uint64_t mine = __atomic_load_n(tc->my_node_bits, __ATOMIC_ACQUIRE);
  CHECK(mine == full_mask(L->node), "my node's bitmap %#lx, expected %#lx",
        (unsigned long)mine, (unsigned long)full_mask(L->node));
  int near_on = (qtype == GtcQueueLAWS) && ((laws_shrb_t *)tc->shared_rb)->near_on;
  if (near_on) {
    uint64_t mn = __atomic_load_n(tc->my_node_bits + 1, __ATOMIC_ACQUIRE);
    CHECK(tc->bit_set_near == 1, "own near bit not set after release");
    CHECK(mn == full_mask(L->node), "my node's near bitmap %#lx, expected %#lx",
          (unsigned long)mn, (unsigned long)full_mask(L->node));
  }
  if (L->nnodes > 1) {
    int n = (L->node + 1) % L->nnodes;
    uint64_t w = shmem_uint64_g(tc->node_bits, gtc_node_leader(n));
    CHECK(w == full_mask(n), "node %d bitmap read remotely %#lx, expected %#lx",
          n, (unsigned long)w, (unsigned long)full_mask(n));
  }
  shmem_barrier_all();

  // 2. run everything under laws + peeks; every task must run exactly once
  gtc_process(gtc);
  long *ex = shmem_malloc(sizeof(long)), *tot = shmem_malloc(sizeof(long));
  *ex = executed;
  shmem_barrier_all();
  shmem_long_sum_reduce(SHMEM_TEAM_WORLD, tot, ex, 1);
  CHECK(*tot == (long)NTASKS * npes, "executed %ld tasks, expected %ld", *tot, (long)NTASKS * npes);

  // 3. queues are empty: bits are cleared lazily, every laws_poll progress calls
  for (int i = 0; i <= (int)tc->laws_poll_mask + 1; i++)
    gtc_progress(gtc);
  shmem_barrier_all();
  CHECK(tc->bit_set == 0, "own bit still set with an empty queue");
  CHECK(tc->bit_set_near == 0, "own near bit still set with an empty queue");
  mine = __atomic_load_n(tc->my_node_bits, __ATOMIC_ACQUIRE);
  CHECK(mine == 0, "my node's bitmap %#lx after the run, expected 0", (unsigned long)mine);
  mine = __atomic_load_n(tc->my_node_bits + 1, __ATOMIC_ACQUIRE);
  CHECK(mine == 0, "my node's near bitmap %#lx after the run, expected 0", (unsigned long)mine);

  int *err = shmem_malloc(sizeof(int)), *etot = shmem_malloc(sizeof(int));
  *err = errors;
  shmem_barrier_all();
  shmem_int_sum_reduce(SHMEM_TEAM_WORLD, etot, err, 1);
  if (me == 0) {
    printf("laws: %ld tasks on %d PEs, %d nodes, queue %s\n", *tot, npes, L->nnodes, gtc_queue_name(gtc));
    if (*etot == 0) printf("LAWS test: SUCCESS\n");
    else            printf("LAWS test: FAILURE (%d errors)\n", *etot);
  }
  gtc_print_stats(gtc);

  gtc_destroy(gtc);
  gtc_fini();
  return 0;
}
