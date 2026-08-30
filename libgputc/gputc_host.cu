/** gputc_host.cu -- host-side lifecycle, class registry, seeding, statistics
 *
 *  Prototype port of libtc/init.c, handle.c, task.c and the host half of
 *  collection-saws.c.
 *  (c) 2026 D. Brian Larkins
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cuda_runtime.h>
#include <nvshmem.h>
#include <nvshmemx.h>

#include "gputc.h"
#include "gputc_types.cuh"
#include "gpu_shrb.cuh"
#include "gpu_termination.cuh"
#include "gputc_internal.cuh"

/* Device-side class registry.  Declared extern in gpu_task.cuh. */
__constant__ gputc_task_fn_t g_class_fn[GPUTC_MAX_CLASSES];
__constant__ int             g_class_body_size[GPUTC_MAX_CLASSES];
__constant__ int             g_nclasses;

static gputc_ctx_t g_collections[GPUTC_MAX_COLLECTIONS];
static int         g_initialized = 0;

#define CUDA_CHECK(call)                                                       \
  do {                                                                         \
    cudaError_t _e = (call);                                                   \
    if (_e != cudaSuccess) {                                                   \
      fprintf(stderr, "%s:%d CUDA error: %s\n", __FILE__, __LINE__,            \
              cudaGetErrorString(_e));                                         \
      abort();                                                                 \
    }                                                                          \
  } while (0)


gputc_ctx_t *gputc_lookup(gputc_t gtc)
{
  if (gtc < 0 || gtc >= GPUTC_MAX_COLLECTIONS) return NULL;
  return g_collections[gtc].in_use ? &g_collections[gtc] : NULL;
}


double gputc_wtime(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}


extern "C" void gputc_cfg_default(gputc_cfg_t *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->nblocks           = 0;      /* 0 => size to the device                */
  cfg->threads_per_block = 256;
  cfg->ring_size         = 8192;
  cfg->steal_retries     = GPUTC_STEAL_RETRIES;
  cfg->coherence_aware   = 1;
  cfg->verbose           = 0;
}


/*==================== INIT / FINI ====================*/

extern "C" int gputc_init(void)
{
  if (g_initialized) return 0;

  nvshmem_init();

  int mype_node = nvshmem_team_my_pe(NVSHMEMX_TEAM_NODE);
  CUDA_CHECK(cudaSetDevice(mype_node));

  memset(g_collections, 0, sizeof(g_collections));
  g_initialized = 1;
  return 0;
}


extern "C" void gputc_fini(void)
{
  if (!g_initialized) return;
  nvshmem_finalize();
  g_initialized = 0;
}


extern "C" int gputc_my_pe(void) { return nvshmem_my_pe(); }
extern "C" int gputc_n_pes(void) { return nvshmem_n_pes(); }


/*==================== SIZING THE GRID ====================*/

/** Choose a block count that fills the device without oversubscribing.
 *
 *  A persistent kernel must not launch more blocks than can be simultaneously
 *  resident: a non-resident block would never run, and the blocks that are
 *  resident would spin waiting to steal work that a scheduled-but-not-running
 *  block is holding.  This is the GPU version of the forward-progress
 *  requirement that makes the steal path lock-free in the first place.
 */
static int gputc_default_nblocks(int threads_per_block, size_t shmem_per_block)
{
  int dev; CUDA_CHECK(cudaGetDevice(&dev));

  cudaDeviceProp prop;
  CUDA_CHECK(cudaGetDeviceProperties(&prop, dev));

  int blocks_per_sm = 0;
  CUDA_CHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &blocks_per_sm, (const void *)gputc_persistent_kernel,
      threads_per_block, shmem_per_block));

  if (blocks_per_sm < 1) blocks_per_sm = 1;
  int n = prop.multiProcessorCount * blocks_per_sm;
  return (n > GPUTC_MAX_BLOCKS) ? GPUTC_MAX_BLOCKS : n;
}


/*==================== CREATE / DESTROY ====================*/

extern "C" gputc_t gputc_create(int max_body_size, const gputc_cfg_t *cfg_in,
                                gputc_qtype_t qtype)
{
  int h = -1;
  for (int i = 0; i < GPUTC_MAX_COLLECTIONS; i++)
    if (!g_collections[i].in_use) { h = i; break; }
  if (h < 0) return -1;

  gputc_ctx_t *c = &g_collections[h];
  memset(c, 0, sizeof(*c));
  c->in_use = 1;
  c->qtype  = qtype;

  if (cfg_in) c->cfg = *cfg_in;
  else        gputc_cfg_default(&c->cfg);

  if (qtype != GputcQueueSAWS)
    fprintf(stderr, "[gputc] warning: only GputcQueueSAWS is implemented, "
                    "falling back to it\n");

  c->my_pe = nvshmem_my_pe();
  c->n_pes = nvshmem_n_pes();

  /* Task slot size: header plus the largest body, rounded to 16 bytes so that
   * a warp-wide read of consecutive descriptors stays aligned and coalesced. */
  c->elem_size = (int)sizeof(gpu_task_t) + max_body_size;
  c->elem_size = (c->elem_size + 15) & ~15;

  if (c->cfg.ring_size > GPUTC_MAX_RING) {
    fprintf(stderr, "[gputc] ring_size %d exceeds the %d addressable by the "
                    "steal word's index fields; clamping\n",
            c->cfg.ring_size, GPUTC_MAX_RING);
    c->cfg.ring_size = GPUTC_MAX_RING;
  }

  size_t shmem_per_block = (size_t)(c->cfg.threads_per_block / GPUTC_WARP_SIZE)
                         * GPUTC_WARP_SIZE * (size_t)c->elem_size;

  if (c->cfg.nblocks <= 0)
    c->cfg.nblocks = gputc_default_nblocks(c->cfg.threads_per_block, shmem_per_block);

  /* Classes are registered after create(); allocate for the maximum and trim
   * the dev_ctx view once the count is known. */
  c->nclasses = GPUTC_MAX_CLASSES;
  c->nqueues  = c->cfg.nblocks * c->nclasses;

  /* ---- allocate the symmetric queues ----
   *
   * Every PE must make identical allocations in identical order so the
   * resulting addresses are symmetric.  That is what lets a thief use its own
   * pointer as the remote address. */
  size_t qbytes = sizeof(gpu_shrb_t) + (size_t)c->elem_size * c->cfg.ring_size;

  c->h_queues = (gpu_shrb_t **)calloc(c->nqueues, sizeof(gpu_shrb_t *));

  for (int i = 0; i < c->nqueues; i++) {
    gpu_shrb_t *q = (gpu_shrb_t *)nvshmem_malloc(qbytes);
    if (!q) { fprintf(stderr, "[gputc] nvshmem_malloc failed\n"); abort(); }

    /* Initialize on the host and push down; simpler than an init kernel and
     * this is not on any hot path. */
    gpu_shrb_t init;
    memset(&init, 0, sizeof(init));
    init.max_size   = c->cfg.ring_size;
    init.elem_size  = c->elem_size;
    init.owner_pe   = c->my_pe;
    init.owner_blk  = i / c->nclasses;
    init.task_class = i % c->nclasses;
    init.cur        = 0;
    init.last       = 0;
    init.steal_val  = gputc_pack_stealval(GPUTC_EPOCH_CLOSED, 0, 0);

    CUDA_CHECK(cudaMemcpy(q, &init, sizeof(init), cudaMemcpyHostToDevice));
    c->h_queues[i] = q;
  }

  CUDA_CHECK(cudaMalloc(&c->d_queues, c->nqueues * sizeof(gpu_shrb_t *)));
  CUDA_CHECK(cudaMemcpy(c->d_queues, c->h_queues,
                        c->nqueues * sizeof(gpu_shrb_t *), cudaMemcpyHostToDevice));

  /* ---- termination and accounting state ---- */
  CUDA_CHECK(cudaMalloc(&c->d_active_blocks, sizeof(unsigned int)));
  CUDA_CHECK(cudaMalloc(&c->d_spawned,       sizeof(unsigned long long)));
  CUDA_CHECK(cudaMalloc(&c->d_completed,     sizeof(unsigned long long)));
  CUDA_CHECK(cudaMalloc(&c->d_terminated,    sizeof(int)));
  CUDA_CHECK(cudaMemset(c->d_active_blocks, 0, sizeof(unsigned int)));
  CUDA_CHECK(cudaMemset(c->d_spawned,       0, sizeof(unsigned long long)));
  CUDA_CHECK(cudaMemset(c->d_completed,     0, sizeof(unsigned long long)));
  CUDA_CHECK(cudaMemset(c->d_terminated,    0, sizeof(int)));

  /* The vote buffers must be symmetric: they are reduction operands. */
  c->d_vote = (unsigned long long *)nvshmem_malloc(2 * sizeof(unsigned long long));
  c->h_vote = (unsigned long long *)nvshmem_malloc(2 * sizeof(unsigned long long));

  CUDA_CHECK(cudaStreamCreate(&c->stream));

  c->dev_ctx.queues        = c->d_queues;
  c->dev_ctx.my_pe         = c->my_pe;
  c->dev_ctx.n_pes         = c->n_pes;
  c->dev_ctx.nblocks       = c->cfg.nblocks;
  c->dev_ctx.nclasses      = c->nclasses;
  c->dev_ctx.elem_size     = c->elem_size;
  c->dev_ctx.max_size      = c->cfg.ring_size;
  c->dev_ctx.active_blocks = c->d_active_blocks;
  c->dev_ctx.spawned       = c->d_spawned;
  c->dev_ctx.completed     = c->d_completed;
  c->dev_ctx.terminated    = c->d_terminated;

  if (c->cfg.verbose && c->my_pe == 0) {
    printf("[gputc] %d PEs, %d blocks x %d threads, ring %d x %d B, "
           "%zu B smem/block\n",
           c->n_pes, c->cfg.nblocks, c->cfg.threads_per_block,
           c->cfg.ring_size, c->elem_size, shmem_per_block);
  }

  nvshmem_barrier_all();
  return h;
}


extern "C" void gputc_destroy(gputc_t gtc)
{
  gputc_ctx_t *c = gputc_lookup(gtc);
  if (!c) return;

  nvshmem_barrier_all();

  for (int i = 0; i < c->nqueues; i++)
    if (c->h_queues[i]) nvshmem_free(c->h_queues[i]);
  free(c->h_queues);

  cudaFree(c->d_queues);
  cudaFree(c->d_active_blocks);
  cudaFree(c->d_spawned);
  cudaFree(c->d_completed);
  cudaFree(c->d_terminated);
  nvshmem_free(c->d_vote);
  nvshmem_free(c->h_vote);
  cudaStreamDestroy(c->stream);

  c->in_use = 0;
}


/*==================== CLASS REGISTRY ====================*/

/** Read a device function pointer out of its __device__ symbol.
 *
 *  A host-taken &func of a __device__ function is not a usable device address;
 *  the value must come from a symbol that the device itself initialized.  This
 *  is the standard indirection and the reason GPUTC_TASK() declares a
 *  companion _devptr symbol.
 */
extern "C" void *gputc_lookup_devfn(const void *sym)
{
  void *fn = NULL;
  CUDA_CHECK(cudaMemcpyFromSymbol(&fn, *(const void **)&sym, sizeof(fn), 0,
                                  cudaMemcpyDeviceToHost));
  return fn;
}


extern "C" gputc_class_t gputc_task_class_register(gputc_t gtc, int body_size,
                                                   void *dev_fn)
{
  gputc_ctx_t *c = gputc_lookup(gtc);
  if (!c) return -1;

  int cls = -1;
  for (int i = 0; i < GPUTC_MAX_CLASSES; i++)
    if (c->class_fn[i] == NULL) { cls = i; break; }
  if (cls < 0) return -1;

  if ((int)sizeof(gpu_task_t) + body_size > c->elem_size) {
    fprintf(stderr, "[gputc] class body %d B exceeds the %d B slot fixed at "
                    "create time\n", body_size, c->elem_size);
    return -1;
  }

  c->class_fn[cls]        = (gputc_task_fn_t)dev_fn;
  c->class_body_size[cls] = body_size;

  CUDA_CHECK(cudaMemcpyToSymbol(g_class_fn, c->class_fn, sizeof(c->class_fn)));
  CUDA_CHECK(cudaMemcpyToSymbol(g_class_body_size, c->class_body_size,
                                sizeof(c->class_body_size)));
  int n = cls + 1;
  CUDA_CHECK(cudaMemcpyToSymbol(g_nclasses, &n, sizeof(n)));

  nvshmem_barrier_all();
  return cls;
}


/*==================== SEEDING ====================*/

extern "C" int gputc_seed(gputc_t gtc, gputc_class_t cls, const void *body, int blk)
{
  gputc_ctx_t *c = gputc_lookup(gtc);
  if (!c || cls < 0 || cls >= c->nclasses) return -1;
  if (blk < 0 || blk >= c->cfg.nblocks) return -1;

  gpu_shrb_t *q = c->h_queues[blk * c->nclasses + cls];

  /* Read back the header, append at the head, write it back.  Only ever used
   * before the kernel launches, so there is nothing to race with. */
  gpu_shrb_t hdr;
  CUDA_CHECK(cudaMemcpy(&hdr, q, sizeof(hdr), cudaMemcpyDeviceToHost));

  if (hdr.nlocal >= hdr.max_size - 1) return -1;

  unsigned char *staging = (unsigned char *)calloc(1, c->elem_size);
  gpu_task_t *t = (gpu_task_t *)staging;
  t->task_class = (unsigned int)cls;
  t->created_by = ((unsigned int)c->my_pe << 16);
  memcpy(t->body, body, (size_t)c->class_body_size[cls]);

  int head = (hdr.split + hdr.nlocal) % hdr.max_size;
  CUDA_CHECK(cudaMemcpy((unsigned char *)q + offsetof(gpu_shrb_t, q)
                            + (size_t)head * c->elem_size,
                        staging, c->elem_size, cudaMemcpyHostToDevice));
  free(staging);

  hdr.nlocal++;
  CUDA_CHECK(cudaMemcpy(q, &hdr, sizeof(hdr), cudaMemcpyHostToDevice));

  /* Count it, so the termination vote's spawned/completed invariant holds
   * from the very first round. */
  unsigned long long sp = 0;
  CUDA_CHECK(cudaMemcpy(&sp, c->d_spawned, sizeof(sp), cudaMemcpyDeviceToHost));
  sp++;
  CUDA_CHECK(cudaMemcpy(c->d_spawned, &sp, sizeof(sp), cudaMemcpyHostToDevice));

  return 0;
}


/*==================== STATISTICS ====================*/

extern "C" void gputc_get_stats(gputc_t gtc, gputc_stats_t *st)
{
  gputc_ctx_t *c = gputc_lookup(gtc);
  if (!c || !st) return;
  memset(st, 0, sizeof(*st));

  CUDA_CHECK(cudaMemcpy(&st->tasks_spawned,  c->d_spawned,
                        sizeof(unsigned long long), cudaMemcpyDeviceToHost));
  CUDA_CHECK(cudaMemcpy(&st->tasks_executed, c->d_completed,
                        sizeof(unsigned long long), cudaMemcpyDeviceToHost));

  for (int i = 0; i < c->nqueues; i++) {
    gpu_shrb_t hdr;
    CUDA_CHECK(cudaMemcpy(&hdr, c->h_queues[i], sizeof(hdr), cudaMemcpyDeviceToHost));
    st->steals_local   += hdr.nsteals;
    st->steal_failures += hdr.nsteal_fail;
    st->releases       += hdr.nreleased;
    st->reclaims       += hdr.nreclaimed;
  }

  st->walltime = c->walltime;
}


extern "C" void gputc_print_stats(gputc_t gtc)
{
  gputc_ctx_t *c = gputc_lookup(gtc);
  if (!c) return;

  gputc_stats_t st;
  gputc_get_stats(gtc, &st);

  printf("[gputc pe %d] executed=%llu spawned=%llu steals=%llu failed=%llu "
         "released=%llu reclaimed=%llu rounds=%d %.4f s\n",
         c->my_pe,
         (unsigned long long)st.tasks_executed,
         (unsigned long long)st.tasks_spawned,
         (unsigned long long)st.steals_local,
         (unsigned long long)st.steal_failures,
         (unsigned long long)st.releases,
         (unsigned long long)st.reclaims,
         c->rounds, st.walltime);
}
