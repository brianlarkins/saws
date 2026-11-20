/*************************************************************/
/*                                                           */
/*  laws_shrb.c - two-door SAWS queue (LAWS M2)              */
/*                                                           */
/*************************************************************/

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tc.h"
#include "laws_shrb.h"

/*
 * Design doc v0.8, section 4.3.  Ring, oldest to newest:
 *
 *   [tail, fsplit)   far region:  claimed far slots, then the far block (far door, SHMEM atomics)
 *   [fsplit, split)  near region: completed and claimed near slots, then the near block
 *                    (near door, CPU atomics through shmem_ptr, node-mates only)
 *   [split, head]    private part (nlocal tasks)
 *
 * Each door is SAWS's claim protocol on its own steal_val-format word, with its own two
 * completion epochs.  Owner operations:
 *   release        public part empty: ceil(nlocal/2) tasks, the oldest max(1, ceil(rho r)) far,
 *                  the rest near
 *   reacquire      owner dry: near reacquire (CPU only) if the near block has work; otherwise, if
 *                  the far block has work, collapse the near region and run SAWS's far reacquire
 *   refill         near block exhausted, far block not: a new near block from the private part
 *   re-split       far block exhausted, near block not: the near block returns to the private part
 *   reclaim        tail advances over completed far claims; once tail reaches fsplit, tail and
 *                  fsplit advance together over completed near claims
 *
 * Invariants (doc I1-I6): every read-modify-write on a word comes from its own domain; a claim's
 * block lies in one door's block; take-back only after a disable (fetch_or); slots are reused only
 * after every claim covering them has posted completion; the owner never trusts a lagging view of
 * its own far word.  Within the near region, every slot before the older near epoch's block is
 * complete, because an epoch is reused only after its claims are complete.
 */

#define FullQueue  1
#define EmptyQueue 0
#define DISABLE    (((uint64_t)SAWS_MAX_EPOCHS) << 38)

/*==================== steal_val helpers (SAWS format) ====================*/

/* asteals:24 | valid (epoch, +2 when disabled):2 | itasks:19 | tail:19 */
static inline uint64_t sv_make(uint64_t valid, uint64_t itasks, int64_t tail) {
  return (valid << 38) | (itasks << 19) | (uint64_t)tail;
}

static inline uint64_t sv_get(uint64_t sv, uint64_t *asteals, uint64_t *itasks, int64_t *tail) {
  *asteals = (sv >> 40) & 0xFFFFFF;
  *itasks  = (sv >> 19) & 0x7FFFF;
  *tail    = sv & 0x7FFFF;
  return (sv >> 38) & 0x3;
}

/* number of claims that take a block of itasks completely (steal-half recurrence) */
static inline int max_steals(uint64_t itasks) {
  uint64_t curr, total = 0;
  int cnt;
  for (cnt = 0, curr = itasks; total != itasks; cnt++) {
    curr = (curr != 1) ? curr >> 1 : 1;
    total += curr;
    curr = itasks - total;
  }
  return cnt;
}

/* tasks taken by the first n claims of a block of itasks */
static inline uint64_t claimed_by(uint64_t itasks, uint64_t n) {
  uint64_t stolen = 0, left = itasks;
  for (; n > 0 && left > 0; n--) {
    stolen += (left != 1) ? left >> 1 : 1;
    left = itasks - stolen;
  }
  return stolen;
}

/* unclaimed tasks behind a door word (0 if disabled or exhausted) */
static inline uint64_t sv_unclaimed(uint64_t sv) {
  uint64_t asteals, itasks;
  int64_t  t;
  if (sv_get(sv, &asteals, &itasks, &t) >= SAWS_MAX_EPOCHS || itasks == 0)
    return 0;
  if (asteals >= (uint64_t)max_steals(itasks))
    return 0;
  return itasks - claimed_by(itasks, asteals);
}

/*==================== ring helpers ====================*/

static inline int64_t rb_add(laws_shrb_t *rb, int64_t i, int64_t n) { return (i + n) % rb->max_size; }
static inline int64_t rb_sub(laws_shrb_t *rb, int64_t i, int64_t n) {
  int64_t x = (i - n) % rb->max_size;
  return x < 0 ? x + rb->max_size : x;
}
/* slots from a forward to b */
static inline int64_t rb_dist(laws_shrb_t *rb, int64_t a, int64_t b) {
  int64_t d = b - a;
  return d < 0 ? d + rb->max_size : d;
}

int laws_shrb_local_size(laws_shrb_t *rb)  { return rb->nlocal; }
int laws_shrb_public_size(laws_shrb_t *rb) { return (int)rb_dist(rb, rb->tail, rb->split); }
int laws_shrb_far_size(laws_shrb_t *rb)    { return (int)rb_dist(rb, rb->tail, rb->fsplit); }
int laws_shrb_near_size(laws_shrb_t *rb)   { return (int)rb_dist(rb, rb->fsplit, rb->split); }
int laws_shrb_size(void *b) {
  laws_shrb_t *rb = (laws_shrb_t *)b;
  return rb->nlocal + laws_shrb_public_size(rb);
}
int laws_shrb_isempty(laws_shrb_t *rb) { return rb->nlocal == 0 && rb->tail == rb->split; }

static inline int status_ld(laws_epoch_t *e, int i) {
  return __atomic_load_n(&e->status[i], __ATOMIC_ACQUIRE);
}

/* all claims of the epoch have posted completion */
static int epoch_complete(laws_epoch_t *e) {
  uint64_t sum = 0;
  for (int i = 0; i < e->maxsteals; i++) {
    int st = status_ld(e, i);
    if (st == 0)
      return 0;
    sum += st;
  }
  return sum == e->itasks;
}

/* completed prefix of the epoch's claims, in tasks */
static uint64_t epoch_prefix(laws_epoch_t *e) {
  uint64_t sum = 0;
  for (int i = 0; i < e->maxsteals; i++) {
    int st = status_ld(e, i);
    if (st == 0)
      break;
    sum += st;
  }
  return sum;
}

/* spin until the epoch's claims are complete; returns 1 if it had to wait */
static int epoch_wait(laws_epoch_t *e) {
  int waited = 0;
  while (!epoch_complete(e))
    waited = 1;
  return waited;
}

static void laws_fail(laws_shrb_t *rb, const char *where, const char *what) {
  fprintf(stderr, "LAWS_SHRB %d: invariant broken in %s: %s\n", rb->procid, where, what);
  laws_shrb_print(rb);
  abort();
}

/* tail moves forward within the far region only */
static inline void advance_tail(laws_shrb_t *rb, int64_t x, const char *where) {
  int64_t d = rb_dist(rb, rb->tail, x);
  if (d <= rb_dist(rb, rb->tail, rb->fsplit))
    rb->tail = x;
  else if (rb->check)
    laws_fail(rb, where, "tail would pass fsplit");
}

/* with the far region empty, tail and fsplit move forward together within the near region */
static inline void advance_fsplit(laws_shrb_t *rb, int64_t x, const char *where) {
  int64_t d = rb_dist(rb, rb->fsplit, x);
  if (d <= rb_dist(rb, rb->fsplit, rb->split))
    rb->tail = rb->fsplit = x;
  else if (rb->check)
    laws_fail(rb, where, "fsplit would pass split");
}

static inline void note_hwm(laws_shrb_t *rb, int size) {
  if ((tc_counter_t)size > rb->ct[LCHwm])
    rb->ct[LCHwm] = size;
}

/*==================== door state and bits ====================*/

static inline void set_far(laws_shrb_t *rb, int live) {
  rb->far_live = live;
  if (rb->use_bits)
    gtc_laws_set_bit(rb->tc, live);
}

static inline void set_near(laws_shrb_t *rb, int live) {
  rb->near_live = live;
  if (rb->use_bits)
    gtc_laws_set_near_bit(rb->tc, live);
}

/* The owner learned that a door has nothing left to claim (an event).  With eager clearing, or
 * without bits, forget it now; otherwise the maintenance check clears it lazily, as in E2. */
static void door_empty_event(laws_shrb_t *rb, int door) {
  tc_t *tc = rb->tc;
  if (rb->use_bits && !tc->ldbal_cfg.laws_eager)
    return;
  if (door == LAWS_DOOR_FAR) {
    if (rb->use_bits && tc->bit_set) tc->ct.laws_eager_clears++;
    set_far(rb, 0);
  } else {
    if (rb->use_bits && tc->bit_set_near) tc->ct.laws_eager_clears++;
    set_near(rb, 0);
  }
}

/* owner: far block has unclaimed tasks.  The owner's loopback atomic_set may not be visible to its
 * own load yet; if memory does not show the last value written (bits 0-39), use the shadow, whose
 * claim count is 0.  A stale view can only under-count claims (doc I6). */
int laws_shrb_far_stealable(laws_shrb_t *rb) {
  uint64_t mem = __atomic_load_n(&rb->steal_val, __ATOMIC_RELAXED);
  uint64_t sv  = ((mem ^ rb->sv_shadow) & ((1ULL << 40) - 1)) ? rb->sv_shadow : mem;
  return sv_unclaimed(sv) > 0;
}

/* owner: unclaimed tasks behind the near door (a CPU-domain word: the owner's load is exact) */
int laws_shrb_near_unclaimed(laws_shrb_t *rb) {
  if (!rb->near_on)
    return 0;
  return (int)sv_unclaimed(__atomic_load_n(&rb->near_sv, __ATOMIC_ACQUIRE));
}

int laws_shrb_near_stealable(laws_shrb_t *rb) {
  return laws_shrb_near_unclaimed(rb) > 0;
}

/*==================== create / reset / destroy ====================*/

laws_shrb_t *laws_shrb_create(int elem_size, int req_size, tc_t *tc) {
  GTC_ENTRY();
  laws_shrb_t *rb;
  gtc_ldbal_cfg_t *cfg = &tc->ldbal_cfg;
  int max_size = req_size;
  char *s;

  // With a near door, completed near slots can sit behind an unclaimed far block until it drains,
  // and refills stop only at half full, so the ring is twice the request: the application keeps at
  // least the capacity it asked for.  All PEs use the same size (far claims compute wraps with it).
  if (cfg->laws_rho_pm < 1000)
    max_size = 2 * req_size;
  if (max_size >= (1 << 19)) {
    if (req_size >= (1 << 19)) {
      fprintf(stderr, "LAWS_SHRB: queue of %d slots exceeds the 19-bit index of steal_val\n", req_size);
      abort();
    }
    max_size = (1 << 19) - 1;
  }

  rb = shmem_align(LAWS_CL, sizeof(laws_shrb_t) + (size_t)elem_size * max_size);
  assert(rb);
  _c->shmallocsize += sizeof(laws_shrb_t) + (size_t)elem_size * max_size;
  memset(rb, 0, sizeof(laws_shrb_t));

  rb->procid      = shmem_my_pe();
  rb->nproc       = shmem_n_pes();
  rb->elem_size   = elem_size;
  rb->max_size    = max_size;
  rb->req_size    = req_size;
  rb->reclaimfreq = __GTC_RECLAIM_POLLFREQ;
  if ((s = getenv("GTC_RECLAIM_FREQ")))
    rb->reclaimfreq = atoi(s);
  rb->check       = (s = getenv("GTC_LAWS_CHECK")) && atoi(s) != 0;
  // experiment (E3.2 probe): the owner's non-blocking loopback atomic_set of the far word can take
  // milliseconds to land if the owner makes no other SHMEM call; quiet after publishing forces it
  rb->far_sync    = (s = getenv("GTC_LAWS_FAR_SYNC")) && atoi(s) != 0;

  rb->targets     = gtc_calloc(rb->nproc, sizeof(uint32_t));
  rb->tc          = tc;
  rb->near_rb     = gtc_near_ptr_table(rb);
  rb->rho_pm      = cfg->laws_rho_pm;
  rb->refill      = cfg->laws_refill;
  rb->resplit     = cfg->laws_resplit;
  rb->near_on     = rb->rho_pm < 1000 && _c->loc.ptr_ok && _c->loc.node_size > 1;
  rb->use_bits    = cfg->target_selection == TARGET_LAWS;
  rb->maint       = rb->use_bits || rb->near_on;

  laws_shrb_reset(rb);
  shmem_barrier_all();
  GTC_EXIT(rb);
}


void laws_shrb_reset(laws_shrb_t *rb) {
  GTC_ENTRY();
  rb->tail = rb->fsplit = rb->split = 0;
  rb->nlocal     = 0;
  rb->fcur = rb->ncur = 0;
  rb->flast = rb->nlast = 1;
  rb->pushcc = rb->progresscc = 0;
  rb->lawscc = 0;
  rb->far_live = rb->near_live = 0;
  rb->last_doors = 0;
  rb->nrelease = rb->nreacquire = rb->nreccalls = rb->nprogress = rb->nensure = 0;
  rb->ngets = rb->nsteals = rb->nxfer = 0;
  memset(rb->ct, 0, sizeof(rb->ct));

  // both doors start disabled (valid = 3), as SAWS's steal_val does
  rb->steal_val = sv_make(SAWS_MAX_EPOCHS + 1, 0, 0);
  rb->sv_shadow = rb->steal_val;
  __atomic_store_n(&rb->near_sv, sv_make(SAWS_MAX_EPOCHS + 1, 0, 0), __ATOMIC_RELEASE);
  memset(rb->completed, 0, sizeof(rb->completed));
  memset(rb->ncompleted, 0, sizeof(rb->ncompleted));
  rb->completed[rb->flast].done  = 1;
  rb->ncompleted[rb->nlast].done = 1;
  rb->ncompleted[rb->ncur].done  = 1;

  for (int i = 0; i < rb->nproc; i++)
    rb->targets[i] = FullQueue;
  GTC_EXIT();
}


void laws_shrb_destroy(laws_shrb_t *rb) {
  GTC_ENTRY();
  free(rb->targets);
  free(rb->near_rb);
  shmem_free(rb);
  GTC_EXIT();
}


void laws_shrb_print(laws_shrb_t *rb) {
  uint64_t a, it;
  int64_t  t;
  uint64_t fv = sv_get(rb->steal_val, &a, &it, &t);
  printf("laws_shrb %d: tail %"PRId64" fsplit %"PRId64" split %"PRId64" nlocal %d max %d (req %d) near_on %d rho %.3f\n",
         rb->procid, rb->tail, rb->fsplit, rb->split, rb->nlocal, rb->max_size, rb->req_size, rb->near_on, rb->rho_pm / 1000.0);
  printf("  far  sv: valid %"PRIu64" asteals %"PRIu64" itasks %"PRIu64" vtail %"PRId64" (shadow %#"PRIx64") cur %d live %d\n",
         fv, a, it, t, rb->sv_shadow, rb->fcur, rb->far_live);
  for (int e = 0; e < SAWS_MAX_EPOCHS; e++)
    printf("    far  epoch %d: itasks %"PRIu64" vtail %"PRId64" done %d maxsteals %d\n", e,
           rb->completed[e].itasks, rb->completed[e].vtail, rb->completed[e].done, rb->completed[e].maxsteals);
  uint64_t nv = sv_get(__atomic_load_n(&rb->near_sv, __ATOMIC_ACQUIRE), &a, &it, &t);
  printf("  near sv: valid %"PRIu64" asteals %"PRIu64" itasks %"PRIu64" vtail %"PRId64" cur %d live %d\n",
         nv, a, it, t, rb->ncur, rb->near_live);
  for (int e = 0; e < SAWS_MAX_EPOCHS; e++) {
    printf("    near epoch %d: itasks %"PRIu64" vtail %"PRId64" done %d maxsteals %d status", e,
           rb->ncompleted[e].itasks, rb->ncompleted[e].vtail, rb->ncompleted[e].done, rb->ncompleted[e].maxsteals);
    for (int i = 0; i < rb->ncompleted[e].maxsteals; i++)
      printf(" %d", status_ld(&rb->ncompleted[e], i));
    printf("\n");
  }
}


/* owner: check the index and epoch invariants; returns the number of violations */
int laws_shrb_verify(laws_shrb_t *rb, const char *where) {
  int bad = 0;
  int64_t far = rb_dist(rb, rb->tail, rb->fsplit), near = rb_dist(rb, rb->fsplit, rb->split);

#define LAWS_VERIFY(c, msg) do { if (!(c)) { bad++; fprintf(stderr, "LAWS_SHRB %d: %s: %s\n", rb->procid, where, msg); } } while (0)
  LAWS_VERIFY(rb->tail >= 0 && rb->tail < rb->max_size && rb->fsplit >= 0 && rb->fsplit < rb->max_size
              && rb->split >= 0 && rb->split < rb->max_size, "index out of range");
  LAWS_VERIFY(far + near == rb_dist(rb, rb->tail, rb->split), "fsplit outside [tail, split]");
  LAWS_VERIFY(rb->nlocal >= 0 && rb->nlocal + far + near <= rb->max_size, "ring overflow");
  if (far > 0) {
    laws_epoch_t *c = &rb->completed[rb->fcur];
    LAWS_VERIFY(rb_add(rb, c->vtail, c->itasks) == rb->fsplit, "far block does not end at fsplit");
  }
  if (near > 0) {
    laws_epoch_t *c = &rb->ncompleted[rb->ncur];
    LAWS_VERIFY(rb_add(rb, c->vtail, c->itasks) == rb->split, "near block does not end at split");
  }
  if (!rb->near_on)
    LAWS_VERIFY(near == 0, "near region without a near door");
#undef LAWS_VERIFY
  if (bad) {
    laws_shrb_print(rb);
    abort();
  }
  return bad;
}

/*==================== reclaim ====================*/

/* SAWS's reclaim on the far door, with fsplit in the role of SAWS's split */
static void far_reclaim(laws_shrb_t *rb) {
  laws_epoch_t *L = &rb->completed[rb->flast], *C = &rb->completed[rb->fcur];

  if (!L->done) {
    if (!epoch_complete(L))
      return;
    L->done = 1;
    advance_tail(rb, C->vtail, "far_reclaim/last");
  }
  uint64_t sum = epoch_prefix(C);
  if (sum == C->itasks)
    C->done = 1;
  if (sum > 0)
    advance_tail(rb, rb_add(rb, C->vtail, sum), "far_reclaim/cur");
}

/* far region empty (tail == fsplit): advance tail and fsplit together over completed near claims.
 * Every near slot before the current epoch's block is complete once the older epoch is. */
static void near_reclaim(laws_shrb_t *rb) {
  laws_epoch_t *L = &rb->ncompleted[rb->nlast], *C = &rb->ncompleted[rb->ncur];

  if (!L->done) {
    if (!epoch_complete(L))
      return;
    L->done = 1;
  }
  uint64_t sum = epoch_prefix(C);
  if (sum == C->itasks)
    C->done = 1;
  advance_fsplit(rb, rb_add(rb, C->vtail, sum), "near_reclaim");
}

int laws_shrb_reclaim_space(laws_shrb_t *rb) {
  TC_START_HTIMER(rb->tc, reclaim);
  if (rb->tail != rb->fsplit)
    far_reclaim(rb);
  if (rb->tail == rb->fsplit && rb->fsplit != rb->split)
    near_reclaim(rb);
  rb->nreccalls++;
  TC_STOP_HTIMER(rb->tc, reclaim);
  return 0;
}

static void ensure_space(laws_shrb_t *rb, int n) {
  TC_START_HTIMER(rb->tc, ensure);
  rb->nensure++;
  int size = laws_shrb_size(rb);
  if (rb->max_size - size < n) {
    laws_shrb_reclaim_space(rb);
    size = laws_shrb_size(rb);
    if (rb->max_size - size < n) {
      printf("LAWS_SHRB: Error, not enough space in the queue to push %d elements\n", n);
      laws_shrb_print(rb);
      assert(0);
    }
  }
  note_hwm(rb, size + n);
  TC_STOP_HTIMER(rb->tc, ensure);
}

/*==================== release ====================*/

void laws_shrb_release(laws_shrb_t *rb) {
  GTC_ENTRY();
  if (rb->nlocal > 0 && rb->tail == rb->split) {
    TC_START_TIMER(rb->tc, release);
    int r = rb->nlocal / 2 + rb->nlocal % 2;      // SAWS's release size
    int rf = r, rn = 0;

    if (rb->near_on) {                            // far share max(1, ceil(rho r)), rest near
      rf = (int)(((int64_t)r * rb->rho_pm + 999) / 1000);
      if (rf < 1) rf = 1;
      if (rf > r) rf = r;
      rn = r - rf;
    }

    // the public part is empty, so every claim of both doors' epochs is complete
    if (rb->check) {
      if (!rb->completed[rb->flast].done && !epoch_complete(&rb->completed[rb->flast]))
        laws_fail(rb, "release", "far older epoch incomplete");
      if (rb->near_on && !rb->ncompleted[rb->nlast].done && !epoch_complete(&rb->ncompleted[rb->nlast]))
        laws_fail(rb, "release", "near older epoch incomplete");
    }

    laws_epoch_t *fe = &rb->completed[rb->fcur];
    fe->itasks    = rf;
    fe->maxsteals = max_steals(rf);
    fe->done      = 0;
    fe->vtail     = rb->tail;
    memset(fe->status, 0, sizeof(fe->status));
    rb->fsplit = rb_add(rb, rb->tail, rf);

    if (rb->near_on) {
      laws_epoch_t *ne = &rb->ncompleted[rb->ncur];
      ne->itasks    = rn;
      ne->maxsteals = max_steals(rn);
      ne->done      = (rn == 0);
      ne->vtail     = rb->fsplit;
      memset(ne->status, 0, sizeof(ne->status));
      rb->ncompleted[rb->nlast].done = 1;
    }
    rb->split   = rb_add(rb, rb->fsplit, rn);
    rb->nlocal -= r;

    // publish the near door first: a store is visible at once, the far atomic_set lands via the HCA
    if (rb->near_on) {
      __atomic_store_n(&rb->near_sv, sv_make(rb->ncur, rn, rb->fsplit), __ATOMIC_RELEASE);
      if (rn > 0) set_near(rb, 1);
    }
    uint64_t sv = sv_make(rb->fcur, rf, rb->tail);
    rb->sv_shadow = sv;
    shmem_atomic_set(&rb->steal_val, sv, rb->procid);
    if (rb->far_sync) shmem_quiet();
    set_far(rb, 1);

    rb->nrelease++;
    rb->ct[LCRelease]++;
    rb->ct[LCRelFar]  += rf;
    rb->ct[LCRelNear] += rn;
    if (rb->check) laws_shrb_verify(rb, "release");
    TC_STOP_TIMER(rb->tc, release);
  }
  GTC_EXIT();
}

/*==================== take-back: near reacquire, collapse, far reacquire ====================*/

/* owner dry, near block has unclaimed tasks: SAWS's reacquire on the near door, CPU only */
static void near_reacquire(laws_shrb_t *rb) {
  uint64_t asteals, itasks;
  int64_t  vt;
  uint64_t sv = __atomic_fetch_or(&rb->near_sv, DISABLE, __ATOMIC_ACQ_REL);   // I4: disable first
  uint64_t valid = sv_get(sv, &asteals, &itasks, &vt);

  rb->ct[LCNearReacq]++;
  if (valid >= SAWS_MAX_EPOCHS)
    return;

  laws_epoch_t *L = &rb->ncompleted[rb->nlast], *C = &rb->ncompleted[rb->ncur];
  if (!L->done) {                                 // the older epoch is about to be reused
    if (epoch_wait(L)) rb->ct[LCWaited]++;
    L->done = 1;
  }

  uint64_t ms = max_steals(itasks);
  uint64_t nclaims = asteals < ms ? asteals : ms;
  uint64_t stolen = claimed_by(itasks, nclaims), left = itasks - stolen;

  if (left == 0) {                                // everything was claimed: the door stays disabled
    door_empty_event(rb, LAWS_DOOR_NEAR);
    return;
  }

  uint64_t amount = (left == 1) ? 1 : left / 2 + left % 2;
  rb->nlocal += amount;
  rb->split   = rb_sub(rb, rb->split, amount);

  // the current epoch keeps only its claims; the rest is republished in the other epoch
  C->itasks    = stolen;
  C->maxsteals = nclaims;
  C->done      = (stolen == 0);
  rb->nlast = rb->ncur;
  rb->ncur  = (rb->ncur + 1) % SAWS_MAX_EPOCHS;
  laws_epoch_t *N = &rb->ncompleted[rb->ncur];
  memset(N, 0, sizeof(*N));
  N->itasks    = left - amount;
  N->maxsteals = max_steals(left - amount);
  N->done      = (left == amount);
  N->vtail     = rb_add(rb, C->vtail, stolen);

  __atomic_store_n(&rb->near_sv, sv_make(rb->ncur, left - amount, N->vtail), __ATOMIC_RELEASE);
  if (left > amount)
    set_near(rb, 1);
  else
    door_empty_event(rb, LAWS_DOOR_NEAR);
  rb->ct[LCNearReacqTasks] += amount;
}


/* owner dry, near block has nothing unclaimed: wait for its claims and give the near region back,
 * so the far block borders the (empty) private part again */
static void collapse(laws_shrb_t *rb) {
  uint64_t asteals, itasks;
  int64_t  vt;
  laws_epoch_t *L = &rb->ncompleted[rb->nlast], *C = &rb->ncompleted[rb->ncur];
  uint64_t sv = __atomic_load_n(&rb->near_sv, __ATOMIC_ACQUIRE);
  uint64_t valid = sv_get(sv, &asteals, &itasks, &vt);
  int waited = 0;

  if (valid < SAWS_MAX_EPOCHS) {
    sv = __atomic_fetch_or(&rb->near_sv, DISABLE, __ATOMIC_ACQ_REL);
    valid = sv_get(sv, &asteals, &itasks, &vt);
    // nothing was claimable at the load, and only the owner adds tasks, so all are claimed
    if (valid < SAWS_MAX_EPOCHS && itasks > 0 && asteals < (uint64_t)max_steals(itasks) && rb->check)
      laws_fail(rb, "collapse", "near block had unclaimed tasks");
  }
  if (!L->done) { waited |= epoch_wait(L); L->done = 1; }
  waited |= epoch_wait(C);
  if (waited) rb->ct[LCWaited]++;

  rb->split = rb->fsplit;
  memset(C, 0, sizeof(*C));
  C->vtail = rb->fsplit;
  C->done  = 1;
  rb->ct[LCCollapse]++;
  door_empty_event(rb, LAWS_DOOR_NEAR);
}


/* SAWS's reacquire on the far door, with fsplit in the role of SAWS's split.
 * Precondition: nlocal == 0 and the near region is empty (split == fsplit). */
static void far_reacquire(laws_shrb_t *rb) {
  uint64_t asteals, itasks, sv;
  int64_t  vt;

  TC_START_TIMER(rb->tc, reacquire);
  rb->nreacquire++;
  rb->ct[LCFarReacq]++;

  sv = shmem_atomic_fetch_or(&rb->steal_val, DISABLE, rb->procid);
  sv_get(sv, &asteals, &itasks, &vt);

  laws_epoch_t *L = &rb->completed[rb->flast];
  if (!L->done && epoch_wait(L))                  // wait for the older epoch before reusing it
    rb->ct[LCWaited]++;

  uint64_t stolen = claimed_by(itasks, asteals), left = itasks - stolen;
  uint64_t amount = (left == 1) ? 1 : left / 2 + left % 2;

  if (amount > 0) {
    rb->nlocal += amount;
    rb->fsplit  = rb_sub(rb, rb->fsplit, amount);
    rb->split   = rb->fsplit;

    // switch epochs (SAWS)
    rb->fcur  = (rb->fcur + 1) % SAWS_MAX_EPOCHS;
    rb->flast = (rb->flast + 1) % SAWS_MAX_EPOCHS;
    laws_epoch_t *C = &rb->completed[rb->fcur];
    L = &rb->completed[rb->flast];
    memset(C, 0, sizeof(*C));

    // tail over the completed claims of the old current epoch
    advance_tail(rb, rb_add(rb, L->vtail, epoch_prefix(L)), "far_reacquire");

    L->itasks    = stolen;
    L->maxsteals = asteals;                       // left > 0, so every counted claim succeeded
    C->itasks    = left - amount;
    C->maxsteals = max_steals(left - amount);
    C->done      = 0;
    C->vtail     = rb_add(rb, L->vtail, L->itasks);
    sv = sv_make(rb->fcur, left - amount, C->vtail);
    rb->ct[LCFarReacqTasks] += amount;
  } else {
    sv = sv_make(rb->fcur, 0, rb->tail);
  }

  rb->sv_shadow = sv;
  shmem_atomic_set(&rb->steal_val, sv, rb->procid);
  if (rb->far_sync) shmem_quiet();
  if (left > amount)
    set_far(rb, 1);
  else
    door_empty_event(rb, LAWS_DOOR_FAR);
  TC_STOP_TIMER(rb->tc, reacquire);
}


void laws_shrb_reacquire(laws_shrb_t *rb) {
  GTC_ENTRY();
  if (rb->nlocal != 0 || rb->tail == rb->split)
    GTC_EXIT();

  // near door first: a CPU-only take-back
  if (rb->near_on && rb->fsplit != rb->split && laws_shrb_near_stealable(rb)) {
    near_reacquire(rb);
    if (rb->check) laws_shrb_verify(rb, "near_reacquire");
    if (rb->nlocal > 0)
      GTC_EXIT();
  }

  // far door: only if the far block has unclaimed tasks (a stale view errs toward yes)
  if (rb->tail != rb->fsplit && laws_shrb_far_stealable(rb)) {
    if (rb->fsplit != rb->split) {
      collapse(rb);
      if (rb->check) laws_shrb_verify(rb, "collapse");
    }
    far_reacquire(rb);
    if (rb->check) laws_shrb_verify(rb, "far_reacquire");
  }
  GTC_EXIT();
}

/*==================== refill, re-split, maintenance ====================*/

/* near block exhausted, far block not: publish a new near block from the oldest private tasks.
 * Preconditions (checked by laws_shrb_maintain): near_on, nlocal >= 2. */
void laws_shrb_refill(laws_shrb_t *rb) {
  if (!rb->near_on || rb->nlocal < 1 || laws_shrb_near_unclaimed(rb) > 0)
    return;
  if (laws_shrb_size(rb) > rb->max_size / 2) {
    rb->ct[LCRefillSpace]++;
    return;
  }
  laws_epoch_t *L = &rb->ncompleted[rb->nlast];
  if (!L->done) {                                 // the new block takes the older epoch
    if (!epoch_complete(L)) {
      rb->ct[LCRefillEpoch]++;
      return;
    }
    L->done = 1;
  }

  int half = rb->nlocal / 2 + rb->nlocal % 2;
  int n = (int)(((int64_t)(1000 - rb->rho_pm) * half) / 1000);  // the near share of a release
  if (n < 1) n = 1;
  if (n > rb->nlocal) n = rb->nlocal;

  // the exhausted current epoch keeps its claims and becomes the older one
  rb->nlast = rb->ncur;
  rb->ncur  = (rb->ncur + 1) % SAWS_MAX_EPOCHS;
  laws_epoch_t *N = &rb->ncompleted[rb->ncur];
  memset(N, 0, sizeof(*N));
  N->itasks    = n;
  N->maxsteals = max_steals(n);
  N->vtail     = rb->split;
  rb->split    = rb_add(rb, rb->split, n);
  rb->nlocal  -= n;

  __atomic_store_n(&rb->near_sv, sv_make(rb->ncur, n, N->vtail), __ATOMIC_RELEASE);
  set_near(rb, 1);
  rb->ct[LCRefill]++;
  rb->ct[LCRefillTasks] += n;
  rb->ct[LCRelNear]     += n;
  if (rb->check) laws_shrb_verify(rb, "refill");
}


/* far block exhausted, near block not: disable the near door and return its unclaimed tasks to the
 * private part, so the next release re-divides the work by rho */
void laws_shrb_resplit(laws_shrb_t *rb) {
  uint64_t asteals, itasks;
  int64_t  vt;
  if (!rb->near_on || rb->fsplit == rb->split)
    return;
  uint64_t sv = __atomic_fetch_or(&rb->near_sv, DISABLE, __ATOMIC_ACQ_REL);   // I4
  uint64_t valid = sv_get(sv, &asteals, &itasks, &vt);
  if (valid >= SAWS_MAX_EPOCHS)
    return;

  uint64_t ms = max_steals(itasks);
  uint64_t nclaims = asteals < ms ? asteals : ms;
  uint64_t stolen = claimed_by(itasks, nclaims), left = itasks - stolen;
  laws_epoch_t *C = &rb->ncompleted[rb->ncur];
  C->itasks    = stolen;
  C->maxsteals = nclaims;
  C->done      = (stolen == 0);
  rb->split    = rb_sub(rb, rb->split, left);
  rb->nlocal  += left;

  set_near(rb, 0);
  rb->ct[LCResplit]++;
  rb->ct[LCResplitTasks] += left;
  if (rb->check) laws_shrb_verify(rb, "resplit");
}


/* every laws_poll progress calls while a door may hold public work (doc Def. maintenance check) */
void laws_shrb_maintain(laws_shrb_t *rb) {
  tc_t *tc = rb->tc;
  rb->ct[LCMaint]++;

  int far_ok = laws_shrb_far_stealable(rb);
  if (!far_ok && (rb->far_live || tc->bit_set))
    set_far(rb, 0);
  if (!rb->near_on)
    return;

  int near_left = laws_shrb_near_unclaimed(rb);
  if (near_left == 0 && (rb->near_live || tc->bit_set_near))
    set_near(rb, 0);

  if (near_left == 0) {
    if (rb->refill && far_ok && rb->nlocal >= 2)
      laws_shrb_refill(rb);
  } else if (!far_ok && rb->resplit && near_left >= 2) {
    laws_shrb_resplit(rb);
  }
}


/* owner starts a search: count searches that begin with a bit still set, and (laws_eager) clear the
 * bit of each door that has nothing to claim */
void laws_shrb_dry(laws_shrb_t *rb) {
  tc_t *tc = rb->tc;
  if (!tc->bit_set && !tc->bit_set_near)
    return;
  int f = laws_shrb_far_stealable(rb), n = laws_shrb_near_stealable(rb);
  tc->ct.laws_dry_bitset++;
  if (f || n)
    tc->ct.laws_dry_stealable++;
  if (!f && tc->bit_set)
    door_empty_event(rb, LAWS_DOOR_FAR);
  if (!n && tc->bit_set_near)
    door_empty_event(rb, LAWS_DOOR_NEAR);
}

/*==================== push / pop (owner) ====================*/

static inline void copy_in(laws_shrb_t *rb, int64_t slot, void *e, int n) {
  int64_t first = rb->max_size - slot;
  if (first >= n) {
    memcpy(laws_shrb_elem_addr(rb, slot), e, (size_t)n * rb->elem_size);
  } else {
    memcpy(laws_shrb_elem_addr(rb, slot), e, (size_t)first * rb->elem_size);
    memcpy(laws_shrb_elem_addr(rb, 0), laws_shrb_buff_elem_addr(rb, e, first), (size_t)(n - first) * rb->elem_size);
  }
}

void laws_shrb_push_head(laws_shrb_t *rb, int proc, void *e, int size) {
  GTC_ENTRY();
  assert(size <= rb->elem_size);
  assert(proc == rb->procid);
  UNUSED(proc);
  if ((rb->pushcc++ % rb->reclaimfreq) == 0)
    ensure_space(rb, 1);
  int64_t slot = rb_add(rb, rb->split, rb->nlocal);
  rb->nlocal++;
  memcpy(laws_shrb_elem_addr(rb, slot), e, size);
  GTC_EXIT();
}

void laws_shrb_push_n_head(void *b, int proc, void *e, int n) {
  GTC_ENTRY();
  laws_shrb_t *rb = (laws_shrb_t *)b;
  assert(proc == rb->procid);
  UNUSED(proc);
  TC_START_TIMER(rb->tc, pushhead);
  ensure_space(rb, n);
  copy_in(rb, rb_add(rb, rb->split, rb->nlocal), e, n);
  rb->nlocal += n;
  TC_STOP_TIMER(rb->tc, pushhead);
  GTC_EXIT();
}

void *laws_shrb_alloc_head(laws_shrb_t *rb) {
  GTC_ENTRY();
  ensure_space(rb, 1);
  int64_t slot = rb_add(rb, rb->split, rb->nlocal);
  rb->nlocal++;
  GTC_EXIT(laws_shrb_elem_addr(rb, slot));
}

int laws_shrb_pop_head(void *b, int proc, void *buf) {
  GTC_ENTRY();
  laws_shrb_t *rb = (laws_shrb_t *)b;
  UNUSED(proc);
  if (rb->nlocal == 0)
    laws_shrb_reacquire(rb);
  if (rb->nlocal > 0) {
    int64_t slot = rb_add(rb, rb->split, rb->nlocal - 1);
    memcpy(buf, laws_shrb_elem_addr(rb, slot), rb->elem_size);
    rb->nlocal--;
    GTC_EXIT(1);
  }
  GTC_EXIT(0);
}

/*==================== claims (initiator) ====================*/

/* successful claim: stop the dispersion and search timers, as SAWS does */
static inline void claim_found(tc_t *tc) {
  if (!tc->dispersed) {
    TC_STOP_TIMER(tc, dispersion);
  }
  TC_STOP_TIMER(tc, search);
}

/* near door of a node-mate, through shmem_ptr: CPU fetch_add, memcpy, store-release of completion */
static int near_claim(laws_shrb_t *myrb, laws_shrb_t *vrb, void *e) {
  tc_t *tc = myrb->tc;
  uint64_t asteals, itasks, sv;
  int64_t  vt;
  uint64_t t0 = gtc_get_tsctime();

  if (tc->ldbal_cfg.near_peek) {
    sv = __atomic_load_n(&vrb->near_sv, __ATOMIC_ACQUIRE);
    tc->ct.laws_peeks++;
    if (sv_unclaimed(sv) == 0) {
      tc->ct.laws_peek_miss++;
      myrb->ct[LCNearPeekMiss]++;
      myrb->ct[LCNearFail]++;
      return 0;
    }
  }

  // acquire: the block's tasks were stored before the owner's store-release of near_sv, and every
  // later fetch_add continues that release sequence (doc I2)
  sv = __atomic_fetch_add(&vrb->near_sv, 1ULL << 40, __ATOMIC_ACQ_REL);
  uint64_t valid = sv_get(sv, &asteals, &itasks, &vt);
  if (valid >= SAWS_MAX_EPOCHS || asteals >= (uint64_t)max_steals(itasks)) {
    myrb->ct[LCNearFail]++;
    return 0;
  }
  uint64_t stolen = claimed_by(itasks, asteals), left = itasks - stolen;
  int ntasks = (left != 1) ? (int)(left >> 1) : 1;

  claim_found(tc);
  int64_t start = (vt + (int64_t)stolen) % myrb->max_size;
  int64_t first = myrb->max_size - start;
  size_t   esz  = myrb->elem_size;             // same on every PE; vrb's header is the owner's cache line
  if (first >= ntasks) {
    memcpy(e, vrb->q + start * esz, ntasks * esz);
  } else {
    memcpy(e, vrb->q + start * esz, first * esz);
    memcpy(laws_shrb_buff_elem_addr(myrb, e, first), vrb->q, (ntasks - first) * esz);
  }
  // single writer per slot: a store-release orders the copy before the owner reuses the slots
  __atomic_store_n(&vrb->ncompleted[valid].status[asteals], ntasks, __ATOMIC_RELEASE);

  myrb->nsteals++;
  myrb->nxfer += (tc_counter_t)ntasks * myrb->elem_size;
  myrb->ct[LCNearOk]++;
  myrb->ct[LCNearTasks] += ntasks;
  uint64_t dt = gtc_get_tsctime() - t0;
  myrb->ct[LCNearTsc]   += dt;
  tc->timers->poptail.total += dt;
  return ntasks;
}

/* far door: SAWS's claim (NIC fetch_add, getmem, quiet, atomic_add of completion) */
static int far_claim(laws_shrb_t *myrb, int proc, laws_shrb_t *vrb, void *e) {
  tc_t *tc = myrb->tc;
  uint64_t asteals, itasks, sv, maxsteals;
  int64_t  rtail;
  int      mate = gtc_is_near(proc);
  int      ntasks = 0;

  if (tc->ldbal_cfg.near_peek && vrb) {
    sv = __atomic_load_n(&vrb->steal_val, __ATOMIC_ACQUIRE);
    tc->ct.laws_peeks++;
    if (sv_unclaimed(sv) == 0) {
      tc->ct.laws_peek_miss++;
      goto notfound;
    }
  }

  tc_timer_t gotwork;
  TC_INIT_ATIMER(gotwork);
  TC_START_ATIMER(gotwork);

test:
  if (myrb->targets[proc] == FullQueue)
    sv = shmem_atomic_fetch_add(&myrb->steal_val, 1ULL << 40, proc);
  else
    sv = shmem_atomic_fetch(&myrb->steal_val, proc);

  uint64_t valid = sv_get(sv, &asteals, &itasks, &rtail);
  if (valid >= SAWS_MAX_EPOCHS)
    goto notfound;
  maxsteals = max_steals(itasks);
  if (asteals >= maxsteals) {
    myrb->targets[proc] = EmptyQueue;
    goto notfound;
  } else if (myrb->targets[proc] == EmptyQueue) {
    myrb->targets[proc] = FullQueue;
    goto test;
  }

  uint64_t stolen = claimed_by(itasks, asteals), left = itasks - stolen;
  ntasks = (left != 1) ? (int)(left >> 1) : 1;
  claim_found(tc);

  int64_t start = (rtail + (int64_t)stolen) % myrb->max_size;
  int64_t first = myrb->max_size - start;
  if (first >= ntasks) {
    shmem_getmem_nbi(e, laws_shrb_elem_addr(myrb, start), (size_t)ntasks * myrb->elem_size, proc);
  } else {
    shmem_getmem_nbi(e, laws_shrb_elem_addr(myrb, start), (size_t)first * myrb->elem_size, proc);
    shmem_getmem_nbi(laws_shrb_buff_elem_addr(myrb, e, first), laws_shrb_elem_addr(myrb, 0),
                     (size_t)(ntasks - first) * myrb->elem_size, proc);
  }
  shmem_quiet();   // the gets must complete before completion is posted
  shmem_atomic_add(&myrb->completed[valid].status[asteals], ntasks, proc);

  myrb->nsteals++;
  myrb->nxfer += (tc_counter_t)ntasks * myrb->elem_size;
  TC_STOP_ATIMER(gotwork);
  TC_ADD_TIMER(tc, poptail, gotwork);
  if (mate) { myrb->ct[LCFarMateOk]++; myrb->ct[LCFarMateTasks] += ntasks; }
  else      { myrb->ct[LCFarRemOk]++;  myrb->ct[LCFarRemTasks]  += ntasks; }
  return ntasks;

notfound:
  if (mate) myrb->ct[LCFarMateFail]++;
  else      myrb->ct[LCFarRemFail]++;
  return 0;
}

/* one claim attempt on proc's queue: its near door, then its far door, as `doors` allows.
 * Off-node targets (and node-mates without shmem_ptr) have only a far door. */
int laws_shrb_claim(laws_shrb_t *myrb, int proc, int doors, void *e) {
  laws_shrb_t *vrb = NULL;
  int got;

  myrb->ngets++;
  myrb->last_doors = 0;
  if (myrb->near_rb && gtc_is_near(proc))
    vrb = (laws_shrb_t *)myrb->near_rb[_c->loc.node_rank_of[proc]];

  if (vrb && myrb->near_on && (doors & LAWS_DOOR_NEAR)) {
    myrb->last_doors |= LAWS_DOOR_NEAR;
    if ((got = near_claim(myrb, vrb, e)) > 0)
      return got;
  }
  if (doors & LAWS_DOOR_FAR) {
    myrb->last_doors |= LAWS_DOOR_FAR;
    return far_claim(myrb, proc, vrb, e);
  }
  return 0;
}

int laws_shrb_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
  laws_shrb_t *rb = (laws_shrb_t *)b;
  UNUSED(n); UNUSED(steal_vol);
  return laws_shrb_claim(rb, proc, rb->tc->laws_door, e);
}

int laws_shrb_try_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
  return laws_shrb_pop_n_tail(b, proc, n, e, steal_vol);
}
