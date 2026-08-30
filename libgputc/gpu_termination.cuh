/** gpu_termination.cuh -- termination detection
 *
 *  Prototype port of libtc/termination.c to CUDA + NVSHMEM.
 *  (c) 2026 D. Brian Larkins
 *
 *  Termination happens at two scopes, and they want different mechanisms.
 *
 *  WITHIN A DEVICE the blocks share coherent global memory, so no token is
 *  needed.  A single counter of non-idle blocks is sufficient: a block
 *  decrements it on going idle and increments it on acquiring work.  When the
 *  count reaches zero, every block on this GPU is out of work.  This is much
 *  cheaper than the CPU scheme and is exact.
 *
 *  ACROSS DEVICES AND NODES the existing tree-token algorithm applies with no
 *  change to its correctness argument: the spawned/completed counter pair
 *  still bounds the in-flight task count, and a task counted as spawned on one
 *  PE may legitimately execute on another.
 *
 *  This prototype drives the global vote from the HOST, between kernel
 *  launches, rather than from device code.  The persistent kernel exits once
 *  it is locally quiescent and has failed to steal from a sample of remote
 *  PEs; the host then votes, and relaunches if the vote fails.  That costs one
 *  kernel relaunch (~5-10 us) per unsuccessful round, which is irrelevant next
 *  to the microsecond-scale steals it is arbitrating, and it keeps the
 *  hardest-to-debug part of the system in host code where it can be printf'd
 *  and single-stepped.  Moving the vote on-device is a later optimization, not
 *  a design change -- td_t from libtc/termination.h ports directly.
 */

#ifndef __GPU_TERMINATION_CUH__
#define __GPU_TERMINATION_CUH__

#include "gputc_types.cuh"

/** Failed remote steal attempts before a block gives up and exits the kernel.
 *  Too low and the host spins on relaunches; too high and the tail of the
 *  computation burns bandwidth on doomed atomics. */
#define GPUTC_STEAL_RETRIES   64


/** Mark this block as having work.  Idempotent per block; call from lane 0 of
 *  warp 0 with the block's current idle state. */
__device__ __forceinline__
void gputc_td_became_active(gputc_dev_ctx_t *ctx, bool *was_idle)
{
  if (*was_idle) {
    atomicAdd(ctx->active_blocks, 1u);
    *was_idle = false;
  }
}


/** Mark this block as out of work. */
__device__ __forceinline__
void gputc_td_became_idle(gputc_dev_ctx_t *ctx, bool *was_idle)
{
  if (!*was_idle) {
    atomicSub(ctx->active_blocks, 1u);
    *was_idle = true;
  }
}


/** True when no block on this device holds work.
 *
 *  Note this is a snapshot, not a barrier: a block may acquire work by stealing
 *  from a remote PE immediately afterwards.  It is only used as a hint for when
 *  to stop trying, with the authoritative decision made by the host vote.
 */
__device__ __forceinline__
bool gputc_td_device_quiescent(gputc_dev_ctx_t *ctx)
{
  return atomicAdd(ctx->active_blocks, 0u) == 0u;
}


/** Account for a completed task. */
__device__ __forceinline__
void gputc_td_completed(gputc_dev_ctx_t *ctx, unsigned int n)
{
  atomicAdd(ctx->completed, (unsigned long long)n);
}

#endif /* __GPU_TERMINATION_CUH__ */
