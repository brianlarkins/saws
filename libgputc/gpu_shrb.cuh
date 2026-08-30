/** gpu_shrb.cuh -- device-side split ring buffer operations
 *
 *  Prototype port of libtc/saws_shrb.c to CUDA + NVSHMEM.
 *  (c) 2026 D. Brian Larkins
 *
 *  This is the interesting file.  It contains the GPU analogue of the SAWS
 *  steal protocol:
 *
 *      1 atomic fetch-add   -> claims a ticket AND reads the epoch parameters
 *      pure ALU             -> derives this thief's exact byte range
 *      1 warp-collective get-> pulls the tasks with no victim involvement
 *      1 atomic add         -> reports completion
 *
 *  Everything is __device__ inline; there is no host-callable code here.
 *  Host-side construction lives in gpu_shrb.cu.
 *
 *  Two things differ from the CPU original and both are deliberate:
 *
 *    (a) The steal schedule is FLAT rather than geometric, and evaluated in
 *        closed form rather than by replaying a loop.  saws_shrb.c:666 walks
 *        the halving sequence to recover the offset for ticket k, which is
 *        O(log n), divergent, and -- worse -- offers the first thief half the
 *        epoch, far more than a warp can hold.  See gputc_steal_share().
 *
 *    (b) Completion is tracked with two counters instead of a per-ticket
 *        status array, because the GPU claimant count is orders of magnitude
 *        larger than the 22 the CPU version budgets for.
 */

#ifndef __GPU_SHRB_CUH__
#define __GPU_SHRB_CUH__

#include <nvshmem.h>
#include <nvshmemx.h>

#include "gputc_types.cuh"


/*==================== STEAL WORD PACK / UNPACK ====================*/

/** Build a steal word.  Mirrors saws_set_stealval(). */
__host__ __device__ __forceinline__
unsigned long long gputc_pack_stealval(unsigned int epoch, unsigned int itasks,
                                       unsigned int vtail)
{
  return (((unsigned long long)(epoch  & GPUTC_EPOCH_MASK))  << GPUTC_EPOCH_SHIFT)
       | (((unsigned long long)(itasks & GPUTC_ITASKS_MASK)) << GPUTC_ITASKS_SHIFT)
       | (((unsigned long long)(vtail  & GPUTC_VTAIL_MASK))  << GPUTC_VTAIL_SHIFT);
}

/** Decompose a steal word.  Mirrors saws_get_stealval(); returns the epoch. */
__host__ __device__ __forceinline__
unsigned int gputc_unpack_stealval(unsigned long long sv, unsigned int *claimed,
                                   unsigned int *itasks, unsigned int *vtail)
{
  *claimed = (unsigned int)((sv >> GPUTC_CLAIM_SHIFT)  & GPUTC_CLAIM_MASK);
  *itasks  = (unsigned int)((sv >> GPUTC_ITASKS_SHIFT) & GPUTC_ITASKS_MASK);
  *vtail   = (unsigned int)((sv >> GPUTC_VTAIL_SHIFT)  & GPUTC_VTAIL_MASK);
  return    (unsigned int)((sv >> GPUTC_EPOCH_SHIFT)  & GPUTC_EPOCH_MASK);
}


/*==================== THE ALLOCATION SCHEDULE ====================*/
/*
 *  The CPU version gives steal k half of whatever remains, halving all the way
 *  down to single tasks.  Two independent things go wrong with that on a GPU.
 *
 *  First, geometric halving bounds the thief count at ~log2(N).  That is why
 *  SAWS_MAX_STEALS_PER_EPOCH is 22, which is ample for a few hundred PEs and
 *  badly wrong for a device with thousands of resident warps: a 256K-task
 *  epoch admits only ~18 claimants and everyone else spins.
 *
 *  Second, and more fundamentally, halving front-loads enormous shares -- the
 *  first thief is offered half the epoch.  A CPU thief can accept that into a
 *  malloc'd buffer.  A GPU thief cannot: a warp has a fixed staging buffer and
 *  can only execute GPUTC_WARP_SIZE tasks at a time, so it would have to
 *  either truncate its claim (silently orphaning the remainder, since the
 *  offsets of later tickets are computed from the schedule, not from what was
 *  actually taken) or loop, holding the epoch open.
 *
 *  So the GPU schedule is flat rather than geometric.  An epoch is divided
 *  into fixed chunks of GPUTC_STEAL_CHUNK tasks:
 *
 *      offset(k) = k * CHUNK
 *      share(k)  = min(nchunks * CHUNK, itasks - offset(k))
 *
 *  and a thief claims however many consecutive chunks it can hold by adding
 *  that many to the claim counter in its single atomic.  This:
 *
 *    - scales with claimants: ceil(N/CHUNK) tickets, 8192 for a 256K epoch
 *    - never offers more than the thief asked for, so nothing is orphaned
 *    - stays O(1) with no loop and no divergence -- a multiply and a subtract
 *    - lets each tier pick its own granularity, so a high-latency remote steal
 *      can amortize over many chunks while a cheap local one takes just enough
 *
 *  What is given up is the CPU version's steal-half adaptivity, which mattered
 *  there because a steal cost a full network round trip and a worker wanted as
 *  much as possible from it.  The equivalent lever here is nchunks, chosen per
 *  tier by known latency rather than derived from queue occupancy.
 */

/** Tickets required to consume an entire epoch at single-chunk granularity. */
__host__ __device__ __forceinline__
unsigned int gputc_max_tickets(unsigned int itasks)
{
  return (itasks + GPUTC_STEAL_CHUNK - 1) / GPUTC_STEAL_CHUNK;
}


/** Slice belonging to a ticket that claimed 'nchunks' consecutive chunks.
 *
 *  Writes the offset from vtail and the task count.  A share of 0 means the
 *  ticket landed past the end of the epoch, i.e. it was fully claimed before
 *  this thief got there.
 */
__host__ __device__ __forceinline__
void gputc_steal_share(unsigned int itasks, unsigned int ticket,
                       unsigned int nchunks,
                       unsigned int *offset, unsigned int *share)
{
  *offset = 0; *share = 0;

  unsigned int off = ticket * GPUTC_STEAL_CHUNK;
  if (off >= itasks) return;                    /* epoch already exhausted */

  unsigned int avail = itasks - off;
  unsigned int want  = nchunks * GPUTC_STEAL_CHUNK;

  *offset = off;
  *share  = (avail < want) ? avail : want;
}


/*==================== ADDRESSING HELPERS ====================*/

__host__ __device__ __forceinline__
unsigned char *gputc_elem(gpu_shrb_t *q, unsigned int idx)
{
  return q->q + (size_t)idx * (size_t)q->elem_size;
}

/** Is this PE reachable with ordinary loads/stores?  True for the local PE and,
 *  when NVLink peer mapping is available, for other GPUs in the same node.
 *  Lets the steal path skip the NVSHMEM layer for the two cheapest tiers. */
__device__ __forceinline__
bool gputc_is_local_pe(gputc_dev_ctx_t *ctx, int pe)
{
  return pe == ctx->my_pe;
}


/*==================== STEAL (thief side) ====================*/

/** Warp-collective steal from (pe, blk) for a given class.
 *
 *  Called by all 32 lanes of a warp.  Lane 0 performs the ticket atomic and
 *  broadcasts the result; the whole warp cooperates on the transfer.
 *
 *  Returns the number of tasks placed in 'buf' (same on every lane), 0 if the
 *  target had nothing to give.
 *
 *  'buf' must be at least maxtasks * elem_size bytes and, for the NVSHMEM
 *  path, should live in memory registered with the runtime.
 */
__device__ __forceinline__
int gputc_warp_steal(gputc_dev_ctx_t *ctx, gpu_shrb_t *rq, int pe,
                     void *buf, int maxtasks)
{
  const unsigned int lane     = threadIdx.x & (GPUTC_WARP_SIZE - 1);
  const unsigned int fullmask = 0xffffffffu;

  unsigned int epoch = 0, ticket = 0, itasks = 0, vtail = 0;
  unsigned int offset = 0, share = 0;

  /* How many chunks to claim.  Bounded by what the caller's staging buffer can
   * actually hold, so a claim is never larger than the tasks we can take --
   * the schedule computes later tickets' offsets from claims, not from what
   * was transferred, so over-claiming would orphan the difference. */
  const bool   local   = gputc_is_local_pe(ctx, pe);
  unsigned int nchunks = local ? GPUTC_LOCAL_CHUNKS : GPUTC_REMOTE_CHUNKS;
  unsigned int cap     = (unsigned int)maxtasks / GPUTC_STEAL_CHUNK;
  if (cap == 0)      return 0;               /* buffer smaller than a chunk */
  if (nchunks > cap) nchunks = cap;

  /* ---- 1. One atomic claims the chunks and reads the epoch parameters ----
   *
   * This is the whole trick.  The fetch-add returns the pre-increment word, so
   * the thief learns simultaneously: which epoch is live, how many tasks it
   * held, where it starts, and its own position in the steal order.  The
   * victim is not interrupted and does not participate.
   */
  if (lane == 0) {
    unsigned long long inc = (unsigned long long)nchunks << GPUTC_CLAIM_SHIFT;
    unsigned long long sv;

    if (local) {
      /* Intra-device tier: an ordinary global atomic, no NVSHMEM involved. */
      sv = atomicAdd(&rq->steal_val, inc);
    } else {
      /* Inter-device tier: the same operation carried over NVLink/IB. */
      sv = nvshmem_uint64_atomic_fetch_add(&rq->steal_val, inc, pe);
    }

    epoch = gputc_unpack_stealval(sv, &ticket, &itasks, &vtail);

    if (epoch != GPUTC_EPOCH_CLOSED && epoch < GPUTC_MAX_EPOCHS)
      gputc_steal_share(itasks, ticket, nchunks, &offset, &share);
    /* else: leave share == 0, the warp will bail below */
  }

  /* ---- 2. Broadcast the decision to the rest of the warp ---- */
  share  = __shfl_sync(fullmask, share,  0);
  offset = __shfl_sync(fullmask, offset, 0);
  vtail  = __shfl_sync(fullmask, vtail,  0);
  epoch  = __shfl_sync(fullmask, epoch,  0);
  ticket = __shfl_sync(fullmask, ticket, 0);

  if (share == 0) {
    if (lane == 0) atomicAdd(&rq->nsteal_fail, 1ULL);
    return 0;
  }
  /* No clamp here on purpose: nchunks was already bounded by the buffer, so
   * share is guaranteed to fit.  Truncating at this point would orphan tasks,
   * because the offsets of subsequent tickets are derived from claims rather
   * than from what any thief actually transferred. */

  /* ---- 3. Warp-collective transfer, directly out of the victim's ring ----
   *
   * The tasks are contiguous by construction, so this is a fully coalesced
   * read.  The ring may wrap, in which case it becomes two transfers -- same
   * structure as saws_shrb.c:690.
   */
  const unsigned int start    = (vtail + offset) % (unsigned int)rq->max_size;
  const size_t       elem     = (size_t)rq->elem_size;
  const bool         wraps    = (start + share) > (unsigned int)rq->max_size;

  if (!wraps) {
    if (local) {
      /* Peer-visible: plain cooperative copy, one lane per task. */
      for (unsigned int i = lane; i < share; i += GPUTC_WARP_SIZE)
        memcpy((unsigned char *)buf + i * elem, gputc_elem(rq, start + i), elem);
    } else {
      nvshmemx_getmem_nbi_warp(buf, gputc_elem(rq, start), share * elem, pe);
    }
  } else {
    const unsigned int first = (unsigned int)rq->max_size - start;
    const unsigned int rest  = share - first;

    if (local) {
      for (unsigned int i = lane; i < first; i += GPUTC_WARP_SIZE)
        memcpy((unsigned char *)buf + i * elem, gputc_elem(rq, start + i), elem);
      for (unsigned int i = lane; i < rest; i += GPUTC_WARP_SIZE)
        memcpy((unsigned char *)buf + (first + i) * elem, gputc_elem(rq, i), elem);
    } else {
      nvshmemx_getmem_nbi_warp(buf, gputc_elem(rq, start), first * elem, pe);
      nvshmemx_getmem_nbi_warp((unsigned char *)buf + first * elem,
                               gputc_elem(rq, 0), rest * elem, pe);
    }
  }

  /* ---- 4. Wait for the data, then report completion ----
   *
   * The victim cannot advance its tail past this epoch until every issued
   * ticket has retired, so this atomic is what makes space reclamation safe.
   */
  /* 'retired' counts CHUNKS, not thieves, so that it can be compared directly
   * against the claim counter regardless of how many chunks each thief took. */
  if (local) {
    __threadfence();
    if (lane == 0) {
      atomicAdd(&rq->epoch[epoch].taken,   share);
      atomicAdd(&rq->epoch[epoch].retired, nchunks);
      atomicAdd(&rq->nsteals, 1ULL);
    }
  } else {
    nvshmemx_quiet_warp();          /* the nbi gets above must land first */
    if (lane == 0) {
      nvshmem_uint_atomic_add(&rq->epoch[epoch].taken,   share,   pe);
      nvshmem_uint_atomic_add(&rq->epoch[epoch].retired, nchunks, pe);
    }
  }
  __syncwarp();

  return (int)share;
}


/*==================== RELEASE / RECLAIM (victim side) ====================*/

/** Move half of the local-only portion into the shared portion and open a new
 *  epoch over it.  Single-threaded: called by lane 0 of the owning block.
 *
 *  Mirrors saws_shrb_release().  The only structural change is that we must
 *  close the previous epoch before opening a new one, because the epoch id is
 *  what a thief uses to index the completion counters.
 */
__device__ __forceinline__
void gputc_release(gpu_shrb_t *q)
{
  if (q->nlocal <= 0) return;

  /* Shared portion must be empty -- same precondition as the CPU version. */
  unsigned int cur = (unsigned int)q->cur;
  if (!q->epoch[cur].closed && q->epoch[cur].itasks > 0) return;

  unsigned int nshared = (unsigned int)(q->nlocal / 2 + q->nlocal % 2);
  if (nshared == 0) return;

  /* Advance to a fresh epoch slot. */
  q->last = q->cur;
  q->cur  = (q->cur + 1) % GPUTC_MAX_EPOCHS;
  cur     = (unsigned int)q->cur;

  gpu_epoch_t *ep = &q->epoch[cur];
  ep->itasks   = nshared;
  ep->vtail    = (unsigned int)q->tail;
  ep->ntickets = 0;
  ep->retired  = 0;
  ep->taken    = 0;
  ep->closed   = 0;

  q->nlocal -= (int)nshared;
  q->split   = (q->split + (int)nshared) % q->max_size;

  /* Publish.  From this instant remote thieves may claim tickets. */
  __threadfence();
  atomicExch(&q->steal_val, gputc_pack_stealval(cur, nshared, ep->vtail));
  atomicAdd(&q->nreleased, 1ULL);
}


/** Stop accepting steals against the current epoch.
 *
 *  The fetch_or writes GPUTC_EPOCH_CLOSED into the epoch field and returns the
 *  pre-or word, whose 'claimed' count is exactly the number of tickets issued
 *  before the close.  Because both the close and every thief's claim are
 *  atomics on the same word, they linearize: a thief either lands before the
 *  or (and is counted) or sees CLOSED and bails without retiring.  There is no
 *  window in which a thief proceeds uncounted.
 */
__device__ __forceinline__
void gputc_close_epoch(gpu_shrb_t *q)
{
  unsigned int cur = (unsigned int)q->cur;
  gpu_epoch_t *ep  = &q->epoch[cur];
  if (ep->closed || ep->itasks == 0) return;

  unsigned long long sv =
      atomicOr(&q->steal_val,
               ((unsigned long long)GPUTC_EPOCH_CLOSED) << GPUTC_EPOCH_SHIFT);

  unsigned int claimed, itasks, vtail;
  gputc_unpack_stealval(sv, &claimed, &itasks, &vtail);

  /* Tickets past the end of the schedule get share 0 and never retire, so
   * they must not be counted as outstanding. */
  unsigned int maxt = gputc_max_tickets(ep->itasks);
  ep->ntickets = (claimed < maxt) ? claimed : maxt;
  ep->closed   = 1;
  __threadfence();
}


/** Try to advance the tail past a fully drained epoch.
 *
 *  Returns the number of task slots reclaimed.  Mirrors
 *  saws_shrb_reclaim_space(), but the O(maxsteals) prefix scan collapses to a
 *  single comparison because we track counts rather than per-ticket status.
 */
__device__ __forceinline__
int gputc_reclaim(gpu_shrb_t *q)
{
  unsigned int last = (unsigned int)q->last;
  gpu_epoch_t *ep   = &q->epoch[last];

  if (!ep->closed || ep->itasks == 0) return 0;

  unsigned int retired = atomicAdd(&ep->retired, 0u);   /* volatile read */
  if (retired < ep->ntickets) return 0;                 /* steals still in flight */

  unsigned int taken = atomicAdd(&ep->taken, 0u);
  q->tail = (int)((ep->vtail + taken) % (unsigned int)q->max_size);

  ep->itasks = 0;
  ep->closed = 0;
  atomicAdd(&q->nreclaimed, 1ULL);
  __threadfence();
  return (int)taken;
}


/*==================== LOCAL OPERATIONS (owner side) ====================*/
/*
 *  On the CPU these require no synchronization at all: one worker owns the
 *  head.  On the GPU the "owner" is an entire block, so the warps within it
 *  contend for the head and we need block-scoped atomics.  These stay in L1
 *  and never generate cross-SM traffic, so they are far cheaper than the
 *  global atomics on the steal path -- but they are not free, which is the
 *  main accounting difference from the CPU original.
 */

/** Acquire the block-scoped head lock.  Call from a single lane.
 *
 *  Safe to spin here: only one lane of the warp participates, so there is no
 *  intra-warp lock-step deadlock, and the owning block is by definition
 *  resident, so the holder is guaranteed to make progress.  Neither property
 *  would hold for a lock spanning blocks, which is why the steal path stays
 *  lock-free. */
__device__ __forceinline__
void gputc_head_lock(gpu_shrb_t *q)
{
  while (atomicCAS_block(&q->hlock, 0u, 1u) != 0u) {
#if __CUDA_ARCH__ >= 700
    __nanosleep(64);
#endif
  }
  __threadfence_block();
}

__device__ __forceinline__
void gputc_head_unlock(gpu_shrb_t *q)
{
  __threadfence_block();
  atomicExch_block(&q->hlock, 0u);
}


/** Push one task onto the head of the local portion.  Single-threaded. */
__device__ __forceinline__
bool gputc_push_head(gpu_shrb_t *q, const void *task)
{
  int size = q->nlocal + ((q->split - q->tail + q->max_size) % q->max_size);
  if (size >= q->max_size - 1) return false;              /* full */

  unsigned int head = (unsigned int)((q->split + q->nlocal) % q->max_size);
  memcpy(gputc_elem(q, head), task, (size_t)q->elem_size);
  q->nlocal++;
  return true;
}


/** Pop up to 'n' tasks off the head of the local portion.  Single-threaded;
 *  the caller distributes them across the warp. */
__device__ __forceinline__
int gputc_pop_head_n(gpu_shrb_t *q, void *buf, int n)
{
  if (q->nlocal <= 0) return 0;
  int got = (q->nlocal < n) ? q->nlocal : n;

  for (int i = 0; i < got; i++) {
    unsigned int idx = (unsigned int)((q->split + q->nlocal - 1 - i) % q->max_size);
    memcpy((unsigned char *)buf + (size_t)i * q->elem_size,
           gputc_elem(q, idx), (size_t)q->elem_size);
  }
  q->nlocal -= got;
  return got;
}


/** Tasks visible to the owner (local portion only). */
__device__ __forceinline__
int gputc_local_size(gpu_shrb_t *q) { return q->nlocal; }


/** Tasks currently exposed for stealing. */
__device__ __forceinline__
int gputc_shared_size(gpu_shrb_t *q)
{
  unsigned int cur = (unsigned int)q->cur;
  gpu_epoch_t *ep  = &q->epoch[cur];
  if (ep->closed || ep->itasks == 0) return 0;
  unsigned int taken = atomicAdd(&ep->taken, 0u);
  return (taken >= ep->itasks) ? 0 : (int)(ep->itasks - taken);
}

#endif /* __GPU_SHRB_CUH__ */
