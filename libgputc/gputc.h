/** gputc.h -- public host API for the GPU task collection
 *
 *  Prototype port of libtc to CUDA + NVSHMEM.
 *  (c) 2026 D. Brian Larkins
 *
 *  Deliberately mirrors the shape of libtc/tc.h so that a program written
 *  against the CPU collection is recognizable here:
 *
 *      gtc_init()                 ->  gputc_init()
 *      gtc_create()               ->  gputc_create()
 *      gtc_task_class_register()  ->  gputc_task_class_register()
 *      gtc_add()                  ->  gputc_seed()      (host, pre-launch)
 *      gtc_process()              ->  gputc_process()
 *      gtc_print_stats()          ->  gputc_print_stats()
 *      gtc_destroy()/gtc_fini()   ->  gputc_destroy()/gputc_fini()
 *
 *  The significant API change is that tasks are no longer added from the host
 *  during execution.  A task body runs on the device and spawns its children
 *  with gputc_dev_add() (gpu_task.cuh), because the persistent kernel never
 *  returns control to the host until the collection is quiesced.
 */

#ifndef __GPUTC_H__
#define __GPUTC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Opaque handle, same idiom as gtc_t. */
typedef int gputc_t;
typedef int gputc_class_t;

/** Queue variant selector.  Only GputcQueueSAWS is implemented; the others are
 *  named so the dispatch structure matches the CPU library and so the
 *  comparison points are explicit. */
typedef enum {
  GputcQueueSAWS,      /* ticket deque, this prototype                    */
  GputcQueueSDC,       /* lock-based split deque, for baseline comparison */
  GputcQueueLAWS       /* locality-aware variant                          */
} gputc_qtype_t;


/** Runtime configuration.  Zero-initialize and override what you need;
 *  gputc_cfg_default() fills in sensible values. */
typedef struct {
  int nblocks;             /* persistent blocks per PE; 0 = fill the device   */
  int threads_per_block;   /* must be a multiple of 32                        */
  int ring_size;           /* tasks per (block, class) deque                  */
  int steal_retries;       /* failed steals before attempting a termination   */
  int coherence_aware;     /* prefer stealing tasks of the class this warp is
                            * already running (see README, "coherence")       */
  int verbose;
} gputc_cfg_t;

void gputc_cfg_default(gputc_cfg_t *cfg);


/*==================== LIFECYCLE ====================*/

/** Initialize NVSHMEM, bind this PE to a GPU, query the device.  Collective. */
int  gputc_init(void);
void gputc_fini(void);

int  gputc_my_pe(void);
int  gputc_n_pes(void);


/** Create a task collection.  Collective over all PEs.
 *
 *  @param max_body_size  bytes of user payload per task
 *  @param cfg            configuration, or NULL for defaults
 *  @param qtype          queue variant
 */
gputc_t gputc_create(int max_body_size, const gputc_cfg_t *cfg, gputc_qtype_t qtype);
void    gputc_destroy(gputc_t gtc);


/*==================== TASK CLASSES ====================*/

/** Register a task class.  Collective; every PE must register the same classes
 *  in the same order so that class ids agree across the job.
 *
 *  @param body_size  bytes of payload for this class
 *  @param dev_fn     device function pointer, obtained on the host with
 *                    cudaMemcpyFromSymbol() from a __device__ function pointer
 *                    symbol.  The GPUTC_CLASS_FN() macro in gpu_task.cuh
 *                    wraps the incantation.
 */
gputc_class_t gputc_task_class_register(gputc_t gtc, int body_size, void *dev_fn);


/*==================== SEEDING AND EXECUTION ====================*/

/** Place an initial task into a block's queue before the kernel launches.
 *  This is the host-side analogue of gtc_add() and is only valid before
 *  gputc_process().  Tasks created during execution use gputc_dev_add(). */
int gputc_seed(gputc_t gtc, gputc_class_t cls, const void *body, int blk);

/** Launch the persistent kernel and run until global termination.  Collective.
 *  Blocks until every PE's collection is quiesced. */
void gputc_process(gputc_t gtc);


/*==================== STATISTICS ====================*/

typedef struct {
  unsigned long long tasks_executed;
  unsigned long long tasks_spawned;
  unsigned long long steals_local;      /* intra-device                       */
  unsigned long long steals_remote;     /* inter-device, via NVSHMEM          */
  unsigned long long steal_failures;
  unsigned long long releases;
  unsigned long long reclaims;
  double             walltime;
} gputc_stats_t;

void gputc_get_stats(gputc_t gtc, gputc_stats_t *st);
void gputc_print_stats(gputc_t gtc);

#ifdef __cplusplus
}
#endif

#endif /* __GPUTC_H__ */
