/** collection.cu -- the persistent kernel and the host-side process loop
 *
 *  Prototype port of libtc/collection-saws.c to CUDA + NVSHMEM.
 *  (c) 2026 D. Brian Larkins
 *
 *  gtc_get_buf_saws() in the CPU library is a loop that tries the local queue,
 *  then steals, then votes on termination.  The GPU version is the same loop,
 *  but it runs inside a kernel that outlives every individual task and it has
 *  more tiers to try, because the cost of finding work varies by four orders
 *  of magnitude depending on where the work is:
 *
 *      tier 0  own deque, preferred class   ~L1, block-scoped atomic
 *      tier 1  own deque, any class         ~L1  (costs warp coherence)
 *      tier 2  another block, this device   ~L2 global atomic
 *      tier 3  another PE                   ~us, NVSHMEM atomic + RDMA get
 *      tier 4  give up, vote on termination ~kernel relaunch
 *
 *  The tiers are tried in order and the cheap ones absorb most of the
 *  imbalance, which is the entire point of making the balancer hierarchical.
 */

#include <stdio.h>
#include <nvshmem.h>
#include <nvshmemx.h>

#include "gputc.h"
#include "gputc_types.cuh"
#include "gpu_shrb.cuh"
#include "gpu_task.cuh"
#include "gpu_termination.cuh"
#include "gputc_internal.cuh"


/*==================== TARGET SELECTION ====================*/

/** xorshift32.  Each warp keeps its own stream so victim choices decorrelate. */
__device__ __forceinline__ unsigned int gputc_rand(unsigned int *s)
{
  unsigned int x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (*s = x);
}


/** Pick a victim.
 *
 *  Mirrors gtc_select_target() but over a two-level space.  We bias toward
 *  the local device because an intra-device steal is ~1000x cheaper than a
 *  remote one; only after several local failures do we reach across NVSHMEM.
 *  This is the same reasoning as local_search_factor in gtc_ldbal_cfg_t,
 *  applied to a steeper cost hierarchy.
 */
__device__ __forceinline__
void gputc_select_victim(gputc_dev_ctx_t *ctx, unsigned int *rng, int nfail,
                         int *out_pe, int *out_blk)
{
  bool go_remote = (ctx->n_pes > 1) && (nfail > 4) && ((gputc_rand(rng) & 3u) == 0u);

  if (go_remote) {
    int pe = (int)(gputc_rand(rng) % (unsigned int)ctx->n_pes);
    if (pe == ctx->my_pe) pe = (pe + 1) % ctx->n_pes;
    *out_pe = pe;
  } else {
    *out_pe = ctx->my_pe;
  }

  int blk = (int)(gputc_rand(rng) % (unsigned int)ctx->nblocks);
  if (*out_pe == ctx->my_pe && blk == (int)blockIdx.x)
    blk = (blk + 1) % ctx->nblocks;
  *out_blk = blk;
}


/*==================== THE PERSISTENT KERNEL ====================*/

/**
 *  Launched once per round with exactly enough blocks to fill the device.
 *  Runs until this device is quiescent and remote steals have stopped paying.
 *
 *  Shared memory layout: one staging buffer of GPUTC_WARP_SIZE task slots per
 *  warp, so a warp can hold a full complement of tasks and execute them with
 *  one task per lane.
 */
__global__ void gputc_persistent_kernel(gputc_dev_ctx_t ctx, int coherence_aware)
{
  extern __shared__ unsigned char s_stage[];

  const int lane   = threadIdx.x & (GPUTC_WARP_SIZE - 1);
  const int warp   = threadIdx.x / GPUTC_WARP_SIZE;
  const int nwarps = blockDim.x / GPUTC_WARP_SIZE;
  const int esz    = ctx.elem_size;

  unsigned char *mybuf = s_stage + (size_t)warp * GPUTC_WARP_SIZE * esz;

  /* Each warp adopts a preferred class and sticks with it.  This is the
   * coherence half of the scheduling objective: a warp that keeps executing
   * one class keeps its dispatch uniform and its instruction cache hot.  We
   * only abandon the preference when the preferred class is starved. */
  int pref = coherence_aware ? (warp % ctx.nclasses) : 0;

  unsigned int rng = 0x9e3779b9u ^ (blockIdx.x * 2654435761u)
                                 ^ (warp * 40503u) ^ (ctx.my_pe * 2246822519u);

  __shared__ bool s_idle;
  if (threadIdx.x == 0) s_idle = false;      /* counted active at launch */
  __syncthreads();

  bool block_idle = false;
  int  nfail = 0;

  while (true) {
    int n = 0;

    /* ---- TIER 0: my own deque, my preferred class ------------------------
     * The overwhelmingly common case.  Block-scoped atomic, stays in L1. */
    {
      gpu_shrb_t *q = ctx.queues[blockIdx.x * ctx.nclasses + pref];
      if (lane == 0) {
        gputc_head_lock(q);
        n = gputc_pop_head_n(q, mybuf, GPUTC_WARP_SIZE);
        gputc_head_unlock(q);
      }
      n = __shfl_sync(0xffffffffu, n, 0);
    }

    /* ---- TIER 1: my own deque, any other class ---------------------------
     * Costs coherence -- the warp switches class -- but is still far cheaper
     * than reaching off the SM.  We take the hit rather than go looking. */
    if (n == 0) {
      for (int c = 0; c < ctx.nclasses && n == 0; c++) {
        if (c == pref) continue;
        gpu_shrb_t *q = ctx.queues[blockIdx.x * ctx.nclasses + c];
        if (lane == 0) {
          gputc_head_lock(q);
          n = gputc_pop_head_n(q, mybuf, GPUTC_WARP_SIZE);
          gputc_head_unlock(q);
        }
        n = __shfl_sync(0xffffffffu, n, 0);
        if (n > 0) pref = c;                 /* re-anchor the preference */
      }
    }

    /* ---- MAINTENANCE: expose work, retire drained epochs ------------------
     * Done by one warp so the block does not thrash its own steal word.  A
     * block that never releases is invisible to thieves and the collection
     * serializes; a block that releases too eagerly gives away work it could
     * have run locally.  Releasing half, as the CPU version does, is a
     * reasonable default and is where a policy knob belongs. */
    if (warp == 0 && lane == 0) {
      for (int c = 0; c < ctx.nclasses; c++) {
        gpu_shrb_t *q = ctx.queues[blockIdx.x * ctx.nclasses + c];
        gputc_reclaim(q);
        if (gputc_shared_size(q) == 0) {
          gputc_head_lock(q);
          if (q->nlocal > 1) {
            gputc_close_epoch(q);
            gputc_release(q);
          }
          gputc_head_unlock(q);
        }
      }
    }

    /* ---- TIERS 2 & 3: steal ---------------------------------------------
     * One atomic, some arithmetic, one coalesced transfer.  Identical code
     * for the intra-device and inter-PE cases; gputc_warp_steal() picks the
     * cheaper mechanism based on whether the victim is peer-visible. */
    if (n == 0) {
      int pe, blk;
      gputc_select_victim(&ctx, &rng, nfail, &pe, &blk);

      /* Coherence-aware victim selection: ask for our preferred class first,
       * and only settle for whatever the victim has after that fails.  This
       * is the queue-level expression of the load-versus-coherence tension --
       * the most available work is frequently the wrong kind. */
      int first = coherence_aware ? pref : 0;
      for (int i = 0; i < ctx.nclasses && n == 0; i++) {
        int c = (first + i) % ctx.nclasses;

        /* Symmetric addressing, exactly as in the CPU library: because every
         * PE allocated its queues collectively from the symmetric heap, the
         * address of *my* queue for (blk, c) is also the correct address of
         * the remote PE's queue for (blk, c).  We pass our own pointer plus a
         * PE id and NVSHMEM resolves it -- the same idiom as passing `myrb`
         * with `proc` in saws_shrb_pop_n_tail(). */
        gpu_shrb_t *rq = ctx.queues[blk * ctx.nclasses + c];

        n = gputc_warp_steal(&ctx, rq, pe, mybuf, GPUTC_WARP_SIZE);
        if (n > 0) pref = c;
      }
    }

    /* ---- TIER 4: no work anywhere we looked ------------------------------ */
    if (n == 0) {
      nfail++;
      if (lane == 0 && warp == 0) {
        gputc_td_became_idle(&ctx, &block_idle);
        s_idle = true;
      }
      __syncthreads();

      /* Leave the kernel only when this device looks globally out of work and
       * we have paid for enough failed probes to believe it.  The host then
       * runs the authoritative vote; see gpu_termination.cuh. */
      if (nfail > GPUTC_STEAL_RETRIES && gputc_td_device_quiescent(&ctx))
        break;
      if (*ctx.terminated) break;
      continue;
    }

    /* ---- EXECUTE --------------------------------------------------------
     * One task per lane, uniform class across the warp, so the indirect call
     * below does not diverge.  This is what the class-segregated queues buy. */
    nfail = 0;
    if (lane == 0 && warp == 0) gputc_td_became_active(&ctx, &block_idle);

    if (lane < n) {
      gpu_task_t *t = (gpu_task_t *)(mybuf + (size_t)lane * esz);
      gputc_dev_execute(&ctx, t);
    }
    __syncwarp();

    if (lane == 0) gputc_td_completed(&ctx, (unsigned int)n);
  }

  /* Make sure this block is not still counted as active on the way out. */
  if (threadIdx.x == 0) gputc_td_became_idle(&ctx, &block_idle);
}


/*==================== HOST-SIDE PROCESS LOOP ====================*/

/** Global termination vote.
 *
 *  The prototype uses an allreduce over the spawned/completed counters, which
 *  is the same invariant the tree token in libtc/termination.c maintains --
 *  the collection is quiescent when every task ever spawned has completed and
 *  nobody is mid-steal.  Swapping in the tree token is a drop-in replacement
 *  and is what a real implementation should do, since an allreduce is O(log P)
 *  latency but synchronizes every PE on every round.
 */
static int gputc_global_vote(gputc_ctx_t *c)
{
  unsigned long long h_spawned = 0, h_completed = 0;

  cudaMemcpy(&h_spawned,   c->d_spawned,   sizeof(h_spawned),   cudaMemcpyDeviceToHost);
  cudaMemcpy(&h_completed, c->d_completed, sizeof(h_completed), cudaMemcpyDeviceToHost);

  c->h_vote[0] = h_spawned;
  c->h_vote[1] = h_completed;

  nvshmem_ulonglong_sum_reduce(NVSHMEM_TEAM_WORLD, c->d_vote, c->h_vote, 2);
  nvshmem_barrier_all();

  unsigned long long total[2];
  cudaMemcpy(total, c->d_vote, 2 * sizeof(unsigned long long), cudaMemcpyDeviceToHost);

  /* Two consecutive agreeing rounds, exactly as the CPU version requires, to
   * rule out a task in flight between the two counter reads. */
  int quiescent = (total[0] == total[1]);
  int decided   = quiescent && c->last_vote_quiescent
                            && (total[0] == c->last_vote_spawned);

  c->last_vote_quiescent = quiescent;
  c->last_vote_spawned   = total[0];
  return decided;
}


extern "C" void gputc_process(gputc_t gtc)
{
  gputc_ctx_t *c = gputc_lookup(gtc);
  if (!c) return;

  size_t shmem_bytes = (size_t)(c->cfg.threads_per_block / GPUTC_WARP_SIZE)
                     * GPUTC_WARP_SIZE * (size_t)c->elem_size;

  /* Every block starts counted as active so the first quiescence test cannot
   * fire before any block has looked for work. */
  unsigned int nactive = (unsigned int)c->cfg.nblocks;
  cudaMemcpy(c->d_active_blocks, &nactive, sizeof(nactive), cudaMemcpyHostToDevice);

  nvshmem_barrier_all();
  double t0 = gputc_wtime();

  int rounds = 0;
  while (1) {
    void *args[] = { &c->dev_ctx, &c->cfg.coherence_aware };

    /* Collective launch is required for kernels that use NVSHMEM device-side
     * synchronization; it also guarantees the co-residency the steal protocol
     * assumes. */
    nvshmemx_collective_launch((const void *)gputc_persistent_kernel,
                               dim3(c->cfg.nblocks),
                               dim3(c->cfg.threads_per_block),
                               args, shmem_bytes, c->stream);
    cudaStreamSynchronize(c->stream);
    rounds++;

    if (gputc_global_vote(c)) break;

    /* Not terminated: some PE still holds work.  Reset the active count and go
     * around again so idle PEs get another chance to steal it. */
    cudaMemcpy(c->d_active_blocks, &nactive, sizeof(nactive), cudaMemcpyHostToDevice);

    if (c->cfg.verbose && c->my_pe == 0)
      printf("[gputc] round %d: not terminated, relaunching\n", rounds);
  }

  nvshmem_barrier_all();
  c->walltime = gputc_wtime() - t0;
  c->rounds   = rounds;
}
