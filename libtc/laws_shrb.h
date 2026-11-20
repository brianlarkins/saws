/*************************************************************/
/*                                                           */
/*  laws_shrb.h - two-door SAWS queue (LAWS M2)              */
/*                                                           */
/*************************************************************/

#ifndef __LAWS_SHRB_H__
#define __LAWS_SHRB_H__

#include <sys/types.h>
#include <stdint.h>
#include <shmem.h>
#include <tc.h>
#include <saws_shrb.h>   // SAWS_MAX_EPOCHS, SAWS_MAX_STEALS_PER_EPOCH, saws_shrb_claimable()

/*
 * Two-door queue (design doc v0.8, section 4.3).  One ring, oldest to newest:
 *
 *     tail          fsplit           split              head
 *      | far region   | near region    | private part    |
 *
 * The far door is SAWS's steal_val and completion arrays, in the SHMEM atomicity
 * domain: any PE claims with a NIC fetch_add.  The near door is the same claim
 * protocol on its own word near_sv and its own completion arrays, in the CPU
 * domain: only node-mates claim, with CPU atomics through shmem_ptr.  No word is
 * shared by the two doors ("one word, one domain").  rho = 1 (or no shmem_ptr)
 * leaves the near door unused, which is SAWS.
 */

#define LAWS_CL 64   // cache line

typedef struct {
  uint64_t itasks;                             // tasks in this epoch's block (corrected to the claimed ones on a switch)
  int64_t  vtail;                              // first slot of the block
  int      done;                               // all claims of this epoch have posted completion
  int      maxsteals;                          // claims to wait for
  int      status[SAWS_MAX_STEALS_PER_EPOCH];  // claim i writes its task count here when its copy is done
} laws_epoch_t;

/* Owner and initiator counters, summed over PEs by print_gstats (LCHwm: max). */
enum laws_ct_e {
  LCRelease, LCRelFar, LCRelNear,               // releases; tasks they put behind each door
  LCRefill, LCRefillTasks, LCRefillSpace, LCRefillEpoch, // refills; tasks; skipped: ring over half full, older epoch busy
  LCResplit, LCResplitTasks,                    // re-splits; tasks returned to the private part
  LCCollapse,                                   // collapses (owner dry, near block empty)
  LCNearReacq, LCNearReacqTasks,                // near-door reacquires (CPU only); tasks taken back
  LCFarReacq, LCFarReacqTasks,                  // far-door reacquires (SAWS's, loopback atomics)
  LCMaint,                                      // maintenance checks
  LCWaited,                                     // reacquires/collapses that had to wait for claims
  LCNearOk, LCNearFail, LCNearTasks, LCNearPeekMiss, LCNearTsc,   // near claims (initiator side)
  LCFarMateOk, LCFarMateFail, LCFarMateTasks,   // far-door claims on node-mates
  LCFarRemOk, LCFarRemFail, LCFarRemTasks,      // far-door claims on other nodes
  LCHwm,                                        // ring high-water mark (slots in use), max over PEs
  LAWS_NCT
};

struct laws_shrb_s {
  /* ---- owner-private state ---- */
  int64_t           tail;        // oldest slot not yet reclaimed
  int64_t           fsplit;      // far region [tail, fsplit), near region [fsplit, split)
  int64_t           split;       // first private slot
  int               nlocal;      // private part size
  int               procid;
  int               nproc;
  int               max_size;    // ring slots (twice the request when the near door is on, see create)
  int               req_size;    // slots the application asked for
  int               elem_size;
  int               reclaimfreq;
  int               pushcc;
  int               progresscc;
  unsigned          lawscc;      // progress calls, for the maintenance check (every laws_poll)
  int               fcur, flast; // far door epochs
  int               ncur, nlast; // near door epochs
  uint64_t          sv_shadow;   // last far steal_val the owner wrote (its loopback write may lag its loads)
  int               near_on;     // near door in use: rho < 1 and node-mates reachable with shmem_ptr
  int               rho_pm;      // export fraction rho in thousandths
  int               refill;      // GTC_LAWS_REFILL
  int               resplit;     // GTC_LAWS_RESPLIT
  int               maint;       // run the maintenance check (laws policy or near door)
  int               use_bits;    // keep the door bits (laws policy)
  int               far_live;    // far block published non-empty and not yet seen exhausted
  int               near_live;   // same for the near block
  int               check;       // GTC_LAWS_CHECK=1: abort on broken invariants
  int               far_sync;    // GTC_LAWS_FAR_SYNC=1 (experiment): shmem_quiet() after publishing the far word
  int               last_doors;  // initiator: doors the last pop_n_tail tried (LAWS_DOOR_*)
  uint32_t         *targets;     // SAWS's per-target empty/full cache (far claims)
  void            **near_rb;     // shmem_ptr to each node-mate's queue (indexed by node rank), NULL if unreachable
  tc_t             *tc;

  tc_counter_t      nrelease, nreacquire, nreccalls, nprogress, nensure;
  tc_counter_t      ngets, nsteals, nxfer;
  tc_counter_t      ct[LAWS_NCT];

  /* ---- far door: SHMEM domain (NIC atomics) ---- */
  uint64_t          steal_val __attribute__((aligned(LAWS_CL)));          // far_sv
  laws_epoch_t      completed[SAWS_MAX_EPOCHS] __attribute__((aligned(LAWS_CL)));

  /* ---- near door: CPU domain (node-mates, CPU atomics through shmem_ptr) ---- */
  uint64_t          near_sv __attribute__((aligned(LAWS_CL)));
  laws_epoch_t      ncompleted[SAWS_MAX_EPOCHS] __attribute__((aligned(LAWS_CL)));

  u_int8_t          q[0] __attribute__((aligned(LAWS_CL)));  // ring
};
typedef struct laws_shrb_s laws_shrb_t;

laws_shrb_t *laws_shrb_create(int elem_size, int max_size, tc_t *tc);
void        laws_shrb_destroy(laws_shrb_t *rb);
void        laws_shrb_reset(laws_shrb_t *rb);

int         laws_shrb_size(void *b);
int         laws_shrb_local_size(laws_shrb_t *rb);
int         laws_shrb_public_size(laws_shrb_t *rb);
int         laws_shrb_far_size(laws_shrb_t *rb);
int         laws_shrb_near_size(laws_shrb_t *rb);
int         laws_shrb_isempty(laws_shrb_t *rb);

void        laws_shrb_release(laws_shrb_t *rb);
void        laws_shrb_reacquire(laws_shrb_t *rb);
int         laws_shrb_reclaim_space(laws_shrb_t *rb);
void        laws_shrb_maintain(laws_shrb_t *rb);   // the maintenance check (refill, re-split, bits)
void        laws_shrb_refill(laws_shrb_t *rb);     // exported for tests; normally called by maintain
void        laws_shrb_resplit(laws_shrb_t *rb);    // exported for tests; normally called by maintain
void        laws_shrb_dry(laws_shrb_t *rb);        // owner starts a search: count and clear stale bits

int         laws_shrb_far_stealable(laws_shrb_t *rb);   // owner: far block has unclaimed tasks (shadow-safe)
int         laws_shrb_near_stealable(laws_shrb_t *rb);  // owner: near block has unclaimed tasks
int         laws_shrb_near_unclaimed(laws_shrb_t *rb);  // owner: unclaimed tasks behind the near door

void        laws_shrb_push_head(laws_shrb_t *rb, int proc, void *e, int size);
void        laws_shrb_push_n_head(void *b, int proc, void *e, int n);
void       *laws_shrb_alloc_head(laws_shrb_t *rb);
int         laws_shrb_pop_head(void *b, int proc, void *buf);
int         laws_shrb_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);
int         laws_shrb_try_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);
int         laws_shrb_claim(laws_shrb_t *rb, int proc, int doors, void *buf);  // one claim attempt on the given doors

void        laws_shrb_print(laws_shrb_t *rb);
int         laws_shrb_verify(laws_shrb_t *rb, const char *where);   // owner: check invariants; returns # violations

#define laws_shrb_elem_addr(RB, IDX)          ((RB)->q + (size_t)(IDX)*(RB)->elem_size)
#define laws_shrb_buff_elem_addr(RB, E, IDX)  ((u_int8_t*)(E) + (size_t)(IDX)*(RB)->elem_size)

#endif /* __LAWS_SHRB_H__ */
