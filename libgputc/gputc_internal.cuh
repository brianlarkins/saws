/** gputc_internal.cuh -- host-side collection state
 *
 *  (c) 2026 D. Brian Larkins
 *
 *  Roughly the GPU analogue of tc_t plus gtc_context_t from libtc/tc.h,
 *  flattened because most of it only needs to exist on the host.
 */

#ifndef __GPUTC_INTERNAL_CUH__
#define __GPUTC_INTERNAL_CUH__

#include <cuda_runtime.h>

#include "gputc.h"
#include "gputc_types.cuh"

#define GPUTC_MAX_COLLECTIONS  8

struct gputc_ctx_s {
  int              in_use;
  int              my_pe, n_pes;
  int              elem_size;      /* header + largest registered body       */
  int              nclasses;
  gputc_qtype_t    qtype;
  gputc_cfg_t      cfg;

  /* Symmetric queue storage.  h_queues holds host copies of the device
   * pointers (needed for seeding and for reading stats back); d_queues is the
   * device-resident array the kernel indexes. */
  gpu_shrb_t     **h_queues;
  gpu_shrb_t     **d_queues;
  int              nqueues;        /* nblocks * nclasses                      */

  /* Termination / accounting, all in symmetric device memory. */
  unsigned int        *d_active_blocks;
  unsigned long long  *d_spawned;
  unsigned long long  *d_completed;
  int                 *d_terminated;
  unsigned long long  *d_vote;
  unsigned long long  *h_vote;

  /* Two-round vote state, mirroring the CPU token's have_voted logic. */
  int                 last_vote_quiescent;
  unsigned long long  last_vote_spawned;

  /* Class registry, staged into __constant__ before launch. */
  gputc_task_fn_t  class_fn[GPUTC_MAX_CLASSES];
  int              class_body_size[GPUTC_MAX_CLASSES];

  gputc_dev_ctx_t  dev_ctx;
  cudaStream_t     stream;

  double           walltime;
  int              rounds;
};
typedef struct gputc_ctx_s gputc_ctx_t;

gputc_ctx_t *gputc_lookup(gputc_t gtc);
double       gputc_wtime(void);

__global__ void gputc_persistent_kernel(gputc_dev_ctx_t ctx, int coherence_aware);

#endif /* __GPUTC_INTERNAL_CUH__ */
