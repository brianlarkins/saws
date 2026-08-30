/** gpu_task.cuh -- device-side task API
 *
 *  Prototype port of libtc/task.c to CUDA.
 *  (c) 2026 D. Brian Larkins
 *
 *  Two problems the CPU version does not have:
 *
 *  1. FUNCTION POINTERS.  A host-taken address of a __device__ function is not
 *     usable on the device.  The registry therefore holds device function
 *     pointers extracted with cudaMemcpyFromSymbol() from a __device__ symbol,
 *     staged into __constant__ memory before launch.  GPUTC_CLASS_FN() hides
 *     the boilerplate.
 *
 *  2. DISPATCH COHERENCE.  If a warp holds a mixed bag of task classes, the
 *     indirect call diverges and the warp serializes.  Because queues are
 *     segregated by class (one deque per (block, class) pair), a warp always
 *     holds 32 tasks of a single class and the call is uniform.  This is the
 *     structural reason the queue array is two-dimensional; see README.
 */

#ifndef __GPU_TASK_CUH__
#define __GPU_TASK_CUH__

#include "gputc_types.cuh"
#include "gpu_shrb.cuh"

/** Device-side class registry, populated by gputc_task_class_register(). */
extern __constant__ gputc_task_fn_t g_class_fn[GPUTC_MAX_CLASSES];
extern __constant__ int             g_class_body_size[GPUTC_MAX_CLASSES];
extern __constant__ int             g_nclasses;


/** Declare a task entry point and a device symbol holding its address.
 *
 *  Usage:
 *      GPUTC_TASK(producer_task) { ... body uses 'ctx' and 'body' ... }
 *
 *  then on the host:
 *      cls = gputc_task_class_register(gtc, sizeof(mytask_t),
 *                                      GPUTC_CLASS_FN(producer_task));
 */
#define GPUTC_TASK(name)                                                      \
  __device__ void name(gputc_dev_ctx_t *ctx, void *body);                     \
  __device__ gputc_task_fn_t name##_devptr = name;                            \
  __device__ void name(gputc_dev_ctx_t *ctx, void *body)

/** Pull the device address of a GPUTC_TASK out of its symbol.  Host-side. */
#define GPUTC_CLASS_FN(name)  gputc_lookup_devfn((const void *)&name##_devptr)

#ifdef __cplusplus
extern "C"
#endif
void *gputc_lookup_devfn(const void *sym);


/*==================== SPAWNING FROM WITHIN A TASK ====================*/

/** Add a task from device code.
 *
 *  This is the hot path for a recursive workload: every producer task calls it
 *  once per child.  It is deliberately single-threaded (one lane) because the
 *  common case is that each lane spawns its own children independently and
 *  a warp-collective variant would need a ballot to coalesce.
 *
 *  Tasks land in the calling block's own deque for their class, matching the
 *  CPU library's default of gtc_add(gtc, task, me).  Work migrates only by
 *  being stolen, which keeps the spawn path free of any remote traffic.
 *
 *  Returns false if the target deque is full, in which case the caller must
 *  either execute the child inline or drop it.  The prototype's task bodies
 *  execute inline; a production version would spill to a global overflow list.
 */
__device__ __forceinline__
bool gputc_dev_add(gputc_dev_ctx_t *ctx, unsigned int cls, const void *body)
{
  if (cls >= (unsigned int)ctx->nclasses) return false;

  gpu_shrb_t *q = ctx->queues[blockIdx.x * ctx->nclasses + cls];

  /* Build the descriptor in a register-resident staging buffer.  elem_size is
   * bounded by the collection's max body size plus the header. */
  unsigned char staging[256];
  gpu_task_t *t = (gpu_task_t *)staging;
  t->task_class = cls;
  t->created_by = ((unsigned int)ctx->my_pe << 16) | (unsigned int)blockIdx.x;

  int bsz = g_class_body_size[cls];
  memcpy(t->body, body, (size_t)bsz);

  gputc_head_lock(q);
  bool ok = gputc_push_head(q, staging);
  gputc_head_unlock(q);

  if (ok) atomicAdd(ctx->spawned, 1ULL);
  return ok;
}


/** Execute one task.  Called with a uniform class across the warp. */
__device__ __forceinline__
void gputc_dev_execute(gputc_dev_ctx_t *ctx, gpu_task_t *t)
{
  gputc_task_fn_t fn = g_class_fn[t->task_class];
  if (fn) fn(ctx, (void *)t->body);
}

#endif /* __GPU_TASK_CUH__ */
