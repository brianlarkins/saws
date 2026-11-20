/*
 * test_laws_shrb.c -- E3.0/E3.1 tests of the two-door queue (laws_shrb.c).
 *
 *   srun --mpi=pmix -N <nodes> --ntasks-per-node=<ppn> ./test-laws-shrb [-n tasks] [-c k] [-q slots] [-s secs] [-u]
 *
 * Settings come from the environment: GTC_LAWS_RHO, GTC_LAWS_DOORS, GTC_LAWS_REFILL,
 * GTC_LAWS_RESPLIT, GTC_TARGET, GTC_NEAR_PEEK, GTC_LAWS_POLL.  Run with GTC_LAWS_CHECK=1 so the
 * queue aborts on a broken index or epoch invariant.
 *
 * 1. unit (with -u, or by default when PE 1 is PE 0's node-mate and rho = 0.5): PE 0 owns, PE 1
 *    claims.  A scripted sequence checks every operation's indices and which tasks each claim
 *    returns: release split by rho, near and far claims, refill, near reacquire, collapse plus far
 *    reacquire, re-split, and a second release.
 * 2. stress: every PE owns a queue and steals; with -c k only every k-th PE creates tasks, so the
 *    others start empty and contend for the creators' doors.  Each task carries (creator, seq).  A PE creates its
 *    tasks in random bursts, pops and executes them, and at random calls release, reclaim, the
 *    maintenance check, refill and re-split (when their switches are on), and progress.  When it steals it claims from a random
 *    node-mate (near door, far door, or both) or a random off-node PE (far door), or uses the
 *    policy's own target selection.  Executing a task adds 1 at seen[seq] on its creator.  At the
 *    end every seen[] entry must be exactly 1, and the queue's checks must never have fired.
 * Prints "LAWS_SHRB test: SUCCESS" or "LAWS_SHRB test: FAILURE (...)" on rank 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#include <tc.h>
#include <laws_shrb.h>

typedef struct { int creator; int seq; } stask_t;

static int errors = 0;
static gtc_t the_gtc;
static tc_t *tc;
static laws_shrb_t *rb;
static int esz;               // queue element size (task header + body)
static long *seen;            // symmetric: seen[seq] counts executions of my task seq
static long *done_total;      // symmetric, on PE 0: tasks executed so far, all PEs
static long executed = 0, flushed = 0;

#define CHECK(cond, ...) do { if (!(cond)) { errors++; if (errors <= 10) { \
      printf("rank %d: FAILED (line %d): ", _c->rank, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } } while (0)

static inline stask_t *body_of(void *elem) { return (stask_t *)gtc_task_body((task_t *)elem); }
static inline void *elem_at(void *buf, int i) { return (char *)buf + (size_t)i * esz; }

static void fill(void *elem, int seq) {
  task_t *t = (task_t *)elem;
  memset(elem, 0, esz);
  t->task_class = 0;
  t->created_by = _c->rank;
  body_of(elem)->creator = _c->rank;
  body_of(elem)->seq     = seq;
}

static void execute(void *elem) {
  stask_t *b = body_of(elem);
  shmem_long_atomic_inc(&seen[b->seq], b->creator);
  executed++;
}

static void on_alarm(int sig) {
  UNUSED(sig);
  fprintf(stderr, "rank %d: test-laws-shrb timed out inside a queue call (executed %ld)\n", _c->rank, executed);
  laws_shrb_print(rb);
  abort();
}

/*====================================================================*/
/* 1. scripted unit test: PE 0 owner, PE 1 initiator (node-mates)      */
/*====================================================================*/

static int ids_are(void *buf, int n, int first) {   // buf holds seqs first, first+1, ... in any order
  long sum = 0, want = 0;
  int lo = 1 << 30, hi = -1;
  for (int i = 0; i < n; i++) {
    int s = body_of(elem_at(buf, i))->seq;
    sum += s; lo = s < lo ? s : lo; hi = s > hi ? s : hi;
    want += first + i;
  }
  return n == 0 || (lo == first && hi == first + n - 1 && sum == want);
}

static void unit_test(void *buf) {
  int me = _c->rank;
  void *push = malloc((size_t)128 * esz);
  void *one  = malloc(esz);
  int got;

  if (me == 0) {
    for (int i = 0; i < 64; i++) fill(elem_at(push, i), i);
    laws_shrb_push_n_head(rb, me, push, 64);
    laws_shrb_release(rb);
    // r = 32: far gets ceil(0.5 * 32) = 16 (seqs 0..15), near 16 (16..31); private 32..63
    CHECK(laws_shrb_far_size(rb) == 16 && laws_shrb_near_size(rb) == 16 && rb->nlocal == 32,
          "release: far %d near %d local %d, expected 16 16 32", laws_shrb_far_size(rb), laws_shrb_near_size(rb), rb->nlocal);
    CHECK(laws_shrb_near_unclaimed(rb) == 16 && laws_shrb_far_stealable(rb), "release: doors not published");
  }
  shmem_barrier_all();

  if (me == 1) {
    // near claims take 8, 4, 2, 1, 1 of seqs 16..31, oldest first
    int want[5] = { 8, 4, 2, 1, 1 }, first = 16;
    for (int k = 0; k < 5; k++) {
      got = laws_shrb_claim(rb, 0, LAWS_DOOR_NEAR, buf);
      CHECK(got == want[k] && ids_are(buf, got, first), "near claim %d: got %d (first seq %d), expected %d from %d",
            k, got, got ? body_of(buf)->seq : -1, want[k], first);
      for (int i = 0; i < got; i++) execute(elem_at(buf, i));
      first += got;
    }
    got = laws_shrb_claim(rb, 0, LAWS_DOOR_NEAR, buf);
    CHECK(got == 0, "near door should be exhausted, got %d", got);
    // one far claim: 8 of seqs 0..15
    got = laws_shrb_claim(rb, 0, LAWS_DOOR_FAR, buf);
    CHECK(got == 8 && ids_are(buf, got, 0), "far claim: got %d (first %d), expected 8 from 0", got, got ? body_of(buf)->seq : -1);
    for (int i = 0; i < got; i++) execute(elem_at(buf, i));
  }
  shmem_barrier_all();

  if (me == 0) {
    laws_shrb_reclaim_space(rb);
    CHECK(rb->tail == 8 && rb->fsplit == 16 && rb->split == 32, "reclaim: tail %ld fsplit %ld split %ld, expected 8 16 32",
          (long)rb->tail, (long)rb->fsplit, (long)rb->split);
    // near exhausted, far not: refill n = floor(0.5 * ceil(32/2)) = 8 (seqs 32..39)
    laws_shrb_maintain(rb);
    CHECK(rb->ct[LCRefill] == 1 && rb->split == 40 && rb->nlocal == 24 && laws_shrb_near_unclaimed(rb) == 8,
          "refill: refills %lu split %ld local %d near %d, expected 1 40 24 8",
          rb->ct[LCRefill], (long)rb->split, rb->nlocal, laws_shrb_near_unclaimed(rb));
  }
  shmem_barrier_all();

  if (me == 1) {
    got = laws_shrb_claim(rb, 0, LAWS_DOOR_NEAR, buf);
    CHECK(got == 4 && ids_are(buf, got, 32), "near claim after refill: got %d, expected 4 from 32", got);
    for (int i = 0; i < got; i++) execute(elem_at(buf, i));
  }
  shmem_barrier_all();

  if (me == 0) {
    // private 40..63 popped newest first
    for (int s = 63; s >= 40; s--) {
      CHECK(laws_shrb_pop_head(rb, me, one) && body_of(one)->seq == s, "pop: expected seq %d", s);
      execute(one);
    }
    CHECK(rb->nlocal == 0, "private part not empty");
    // dry: near reacquire takes ceil(4/2) = 2 (38, 39), then 1 (37), then 1 (36), CPU only
    int nseq[4] = { 39, 38, 37, 36 };
    for (int k = 0; k < 4; k++) {
      CHECK(laws_shrb_pop_head(rb, me, one) && body_of(one)->seq == nseq[k], "near reacquire pop %d: seq %d, expected %d",
            k, body_of(one)->seq, nseq[k]);
      execute(one);
    }
    CHECK(rb->ct[LCNearReacq] == 3 && rb->ct[LCFarReacq] == 0, "near reacquires %lu far %lu, expected 3 0",
          rb->ct[LCNearReacq], rb->ct[LCFarReacq]);
    // near empty, far has 8..15: collapse (split back to fsplit), then SAWS's far reacquire takes 12..15
    CHECK(laws_shrb_pop_head(rb, me, one) && body_of(one)->seq == 15, "collapse/far reacquire: seq %d, expected 15",
          body_of(one)->seq);
    execute(one);
    CHECK(rb->ct[LCCollapse] == 1 && rb->ct[LCFarReacq] == 1 && rb->fsplit == 12 && rb->split == 12 && rb->nlocal == 3,
          "collapse: collapses %lu far reacq %lu fsplit %ld split %ld local %d, expected 1 1 12 12 3",
          rb->ct[LCCollapse], rb->ct[LCFarReacq], (long)rb->fsplit, (long)rb->split, rb->nlocal);
    while (laws_shrb_pop_head(rb, me, one)) execute(one);
    laws_shrb_reclaim_space(rb);
    CHECK(laws_shrb_isempty(rb), "queue not empty after the first round: tail %ld split %ld local %d",
          (long)rb->tail, (long)rb->split, rb->nlocal);

    // second round: re-split.  64..127; release r = 32: far 64..79, near 80..95
    for (int i = 0; i < 64; i++) fill(elem_at(push, i), 64 + i);
    laws_shrb_push_n_head(rb, me, push, 64);
    laws_shrb_release(rb);
  }
  shmem_barrier_all();

  if (me == 1) {
    int n = 0;
    while ((got = laws_shrb_claim(rb, 0, LAWS_DOOR_FAR, buf)) > 0) {
      for (int i = 0; i < got; i++) execute(elem_at(buf, i));
      n += got;
    }
    CHECK(n == 16, "far claims took %d, expected 16", n);
  }
  shmem_barrier_all();

  if (me == 0) {
    // far exhausted, near has 16: re-split returns them, then reclaim empties the public part
    laws_shrb_maintain(rb);
    CHECK(rb->ct[LCResplit] == 1 && rb->nlocal == 48 && rb->split == rb->fsplit,
          "re-split: resplits %lu local %d split %ld fsplit %ld, expected 1 48 = fsplit",
          rb->ct[LCResplit], rb->nlocal, (long)rb->split, (long)rb->fsplit);
    laws_shrb_reclaim_space(rb);
    CHECK(rb->tail == rb->split, "reclaim after re-split: tail %ld split %ld", (long)rb->tail, (long)rb->split);
    // the next release re-divides: r = 24, far 12 (80..91), near 12 (92..103)
    laws_shrb_release(rb);
    CHECK(laws_shrb_far_size(rb) == 12 && laws_shrb_near_size(rb) == 12 && rb->nlocal == 24,
          "release after re-split: far %d near %d local %d, expected 12 12 24",
          laws_shrb_far_size(rb), laws_shrb_near_size(rb), rb->nlocal);
    while (laws_shrb_pop_head(rb, me, one)) execute(one);
    laws_shrb_reclaim_space(rb);
    CHECK(laws_shrb_isempty(rb), "queue not empty after the second round");
  }
  shmem_barrier_all();

  // every seq 0..127 of PE 0 ran exactly once
  if (me == 0)
    for (int s = 0; s < 128; s++)
      CHECK(seen[s] == 1, "unit: seq %d ran %ld times", s, seen[s]);
  shmem_barrier_all();
  if (me == 0) memset(seen, 0, 128 * sizeof(long));
  executed = 0;
  laws_shrb_reset(rb);
  gtc_laws_set_bit(tc, 0);
  gtc_laws_set_near_bit(tc, 0);
  shmem_barrier_all();
  free(push);
  free(one);
}

/*====================================================================*/
/* 2. randomized stress                                                */
/*====================================================================*/

static void flush_count(void) {
  if (executed > flushed) {
    shmem_long_atomic_add(done_total, executed - flushed, 0);
    flushed = executed;
  }
}

static void stress(int ntasks, int creators_every, int secs, void *buf) {
  int me = _c->rank, npes = _c->size;
  int mine = (me % creators_every == 0) ? ntasks : 0;   // tasks this PE creates
  int ncreators = (npes + creators_every - 1) / creators_every;
  gtc_locality_t *L = &_c->loc;
  void *burst = malloc((size_t)32 * esz);
  void *one   = malloc(esz);
  int created = 0;
  long iter = 0, total = (long)ntasks * ncreators, steals = 0, stolen = 0;
  double t0 = (double)gtc_get_tsctime();
  double limit = secs * _c->tsc_cpu_hz * 1e6;
  gtc_vs_state_t vs = {0};
  int timed_out = 0;
  int ncm = 0, *cmates = malloc(L->node_size * sizeof(int));   // creators on my node, other than me
  for (int i = 0; i < L->node_size; i++)
    if (L->node_pes[i] % creators_every == 0 && L->node_pes[i] != me)
      cmates[ncm++] = L->node_pes[i];

  while (1) {
    int r = rand() % 100;
    iter++;
    if (iter & 1)
      gtc_progress_laws(the_gtc);   // as an application does on every task: release, reclaim, maintenance
    int size = laws_shrb_size(rb), room = rb->max_size - size;

    if (created < mine && r < 20) {
      int n = 1 + rand() % 32;
      if (n > mine - created) n = mine - created;
      if (n <= room) {
        for (int i = 0; i < n; i++) fill(elem_at(burst, i), created + i);
        laws_shrb_push_n_head(rb, me, burst, n);
        created += n;
      }
    } else if (r < 24) {
      laws_shrb_release(rb);
    } else if (r < 28) {
      laws_shrb_reclaim_space(rb);
    } else if (r < 32) {
      laws_shrb_maintain(rb);
    } else if (r < 34) {
      if (rb->refill) laws_shrb_refill(rb);
    } else if (r < 36) {
      if (rb->resplit) laws_shrb_resplit(rb);
    } else if (r < 40) {
      gtc_progress_laws(the_gtc);
    } else if (!laws_shrb_isempty(rb) && r < 90) {
      if (laws_shrb_pop_head(rb, me, one))   // with nothing private this is the owner's reacquire
        execute(one);
    } else if (npes > 1 && room > rb->max_size / 4) {
      int v, doors;
      if (creators_every > 1 && ncm > 0 && rand() % 4) {   // contend for the few creators' doors
        v = cmates[rand() % ncm];
        doors = 1 + rand() % 3;
      } else if (rand() % 2 && tc->ldbal_cfg.target_selection != TARGET_RANDOM) {
        tc->laws_door = LAWS_DOOR_BOTH;
        v = gtc_select_target(the_gtc, &vs);
        doors = tc->laws_door;
      } else if (L->node_size > 1 && (L->far_count == 0 || rand() % 2)) {
        v = gtc_random_near_peer();
        doors = 1 + rand() % 3;   // near, far, or both
      } else {
        v = gtc_random_far_peer();
        doors = LAWS_DOOR_FAR;
      }
      if (v >= 0 && v != me) {
        int got = laws_shrb_claim(rb, v, doors, buf);
        if (got > 0) {
          laws_shrb_push_n_head(rb, me, buf, got);
          steals++;
          stolen += got;
        } else if (gtc_is_near(v)) {
          uint64_t bit = 1ULL << L->node_rank_of[v];
          if (rb->last_doors & LAWS_DOOR_NEAR) vs.near_miss |= bit;
          if (rb->last_doors & LAWS_DOOR_FAR)  vs.near_miss_far |= bit;
        }
        if (rand() % 16 == 0) vs.near_miss = vs.near_miss_far = 0;   // a new "search"
      }
    }

    if ((iter & 255) == 0) {
      if (created == mine && laws_shrb_isempty(rb)) {
        flush_count();
        if (shmem_long_atomic_fetch(done_total, 0) >= total)
          break;
      }
      if ((double)gtc_get_tsctime() - t0 > limit) {
        timed_out = 1;
        break;
      }
    }
  }
  flush_count();
  alarm(0);
  shmem_barrier_all();

  // every task I created ran exactly once
  int missing = 0, dup = 0;
  for (int s = 0; s < mine; s++) {
    if (seen[s] == 0) missing++;
    else if (seen[s] > 1) dup++;
  }
  for (int s = mine; s < ntasks; s++)
    if (seen[s] != 0) dup++;
  CHECK(missing == 0 && dup == 0 && created == mine, "stress: created %d of %d, %d never ran, %d ran twice or more%s",
        created, mine, missing, dup, timed_out ? " (timed out)" : "");

  static long st[8], sts[8];
  st[0] = steals; st[1] = stolen; st[2] = rb->ct[LCRefill]; st[3] = rb->ct[LCResplit];
  st[4] = rb->ct[LCCollapse]; st[5] = rb->ct[LCNearReacq]; st[6] = rb->ct[LCFarReacq]; st[7] = rb->ct[LCRefillSpace];
  long *a = shmem_malloc(8 * sizeof(long)), *b = shmem_malloc(8 * sizeof(long));
  memcpy(a, st, sizeof(st));
  shmem_barrier_all();
  shmem_long_sum_reduce(SHMEM_TEAM_WORLD, b, a, 8);
  memcpy(sts, b, sizeof(sts));
  if (me == 0)
    printf("stress: %d creators of %d PEs x %d tasks, %ld steals (%ld tasks), refill %ld (skipped for space %ld), resplit %ld, "
           "collapse %ld, near reacq %ld, far reacq %ld\n",
           ncreators, npes, ntasks, sts[0], sts[1], sts[2], sts[7], sts[3], sts[4], sts[5], sts[6]);
  shmem_free(a);
  shmem_free(b);
  free(burst);
  free(one);
  free(cmates);
}

int main(int argc, char **argv) {
  int ntasks = 20000, slots = 256, secs = 60, unit = -1, every = 1, arg;

  gtc_init();
  while ((arg = getopt(argc, argv, "n:c:q:s:u")) != -1) {
    switch (arg) {
      case 'n': ntasks = atoi(optarg); break;
      case 'c': every  = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
      case 'q': slots  = atoi(optarg); break;
      case 's': secs   = atoi(optarg); break;
      case 'u': unit   = 1; break;
    }
  }

  gtc_ldbal_cfg_t cfg;
  gtc_ldbal_cfg_init(&cfg);
  gtc_t gtc = gtc_create(sizeof(stask_t), 10, slots, &cfg, GtcQueueLAWS);
  the_gtc = gtc;
  tc  = gtc_lookup(gtc);
  rb  = (laws_shrb_t *)tc->shared_rb;
  esz = rb->elem_size;
  seen = shmem_calloc(ntasks > 128 ? ntasks : 128, sizeof(long));
  done_total = shmem_calloc(1, sizeof(long));
  void *buf = malloc((size_t)rb->max_size * esz);

  if (_c->rank == 0) {
    gtc_print_config(gtc);
    printf("test-laws-shrb: %d PEs, %d nodes, ring %d slots (asked %d), near door %s, check %s\n",
           _c->size, _c->loc.nnodes, rb->max_size, rb->req_size, rb->near_on ? "on" : "off",
           rb->check ? "on" : "OFF (set GTC_LAWS_CHECK=1)");
  }
  signal(SIGALRM, on_alarm);
  alarm(secs + 60);

  // the scripted test needs PE 1 on PE 0's node, a near door, rho = 0.5, and room for 128 tasks
  int can_unit = _c->size > 1 && gtc_is_near(1) && rb->near_on && rb->rho_pm == 500 && rb->max_size >= 256
                 && rb->refill && rb->resplit;
  if (unit < 0) unit = can_unit;
  if (unit && can_unit) {
    unit_test(buf);
    if (_c->rank == 0) printf("unit: %s\n", errors ? "errors (see above)" : "passed");
  } else if (_c->rank == 0) {
    printf("unit: skipped (needs PE 1 on PE 0's node, the near door, rho 0.5, refill and re-split on, -q >= 128)\n");
  }
  shmem_barrier_all();

  stress(ntasks, every, secs, buf);

  int *err = shmem_malloc(sizeof(int)), *etot = shmem_malloc(sizeof(int));
  *err = errors;
  shmem_barrier_all();
  shmem_int_sum_reduce(SHMEM_TEAM_WORLD, etot, err, 1);
  if (_c->rank == 0) {
    if (*etot == 0) printf("LAWS_SHRB test: SUCCESS\n");
    else            printf("LAWS_SHRB test: FAILURE (%d errors)\n", *etot);
  }
  shmem_barrier_all();
  gtc_destroy(gtc);
  gtc_fini();
  return 0;
}
