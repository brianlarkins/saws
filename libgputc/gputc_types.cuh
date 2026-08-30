/** gputc_types.cuh -- core types for the GPU task collection
 *
 *  Prototype port of libtc/SAWS to CUDA + NVSHMEM.
 *  (c) 2026 D. Brian Larkins
 *
 *  This header defines the on-device representation of the split ring buffer
 *  and, most importantly, the layout of the single 64-bit "steal word" that
 *  carries the entire negotiable state of a queue.
 *
 *  The CPU original packs (asteals, epoch, itasks, vtail) into 64 bits and
 *  relies on the fact that a single shmem_atomic_fetch_add() both claims a
 *  steal ticket and returns everything the thief needs to compute its own
 *  slice without further communication.  We keep that property exactly; only
 *  the field widths and the allocation schedule change to accommodate the far
 *  higher claimant count of a GPU.
 */

#ifndef __GPUTC_TYPES_CUH__
#define __GPUTC_TYPES_CUH__

#include <stdint.h>

/*==================== TUNABLES ====================*/

/** Warp width.  Everything about steal granularity is expressed in these. */
#define GPUTC_WARP_SIZE        32

/** Granularity of the steal schedule: an epoch is divided into chunks of this
 *  many tasks and a ticket claims a whole number of them.  One warp's worth is
 *  the natural unit -- a steal returning fewer leaves lanes idle, and a warp
 *  cannot execute more than this at once anyway. */
#define GPUTC_STEAL_CHUNK      GPUTC_WARP_SIZE

/** Chunks claimed by a single ticket, per tier.
 *
 *  A thief may claim several consecutive chunks with one atomic by adding more
 *  than 1 to the claim counter.  Intra-device steals take one chunk because an
 *  L2 atomic is cheap relative to 32 tasks of work; a remote steal pays
 *  microseconds of latency and should amortize it over more tasks.
 *
 *  GPUTC_REMOTE_CHUNKS is 1 in this prototype only because the per-warp
 *  staging buffer is sized at one chunk.  Raising it requires enlarging that
 *  buffer, which costs shared memory and therefore occupancy -- a real tuning
 *  decision that wants measurement, not a guess. */
#define GPUTC_LOCAL_CHUNKS     1
#define GPUTC_REMOTE_CHUNKS    1

/** Number of concurrently tracked steal epochs.  The CPU version uses 2.  We
 *  use more because a GPU has enough concurrent thieves that an epoch can be
 *  drained (and a new release stalled behind it) much more often. */
#define GPUTC_MAX_EPOCHS       8

/** Epoch id written into the steal word to mark "no steals accepted".  Must be
 *  representable in GPUTC_EPOCH_BITS and must not collide with a real epoch. */
#define GPUTC_EPOCH_CLOSED     0xF

/** Maximum number of distinct task classes. */
#define GPUTC_MAX_CLASSES      8

/** Blocks per PE.  Each block owns one deque per class and plays the role that
 *  a PE plays in the CPU system: it is the unit of ownership and of stealing. */
#define GPUTC_MAX_BLOCKS       1024


/*==================== STEAL WORD LAYOUT ====================*/
/*
 *   63                44 43   40 39            20 19             0
 *  +--------------------+-------+----------------+----------------+
 *  |      claimed       | epoch |     itasks     |     vtail      |
 *  +--------------------+-------+----------------+----------------+
 *          20 bits        4 bits      20 bits          20 bits
 *
 *  claimed : number of steal tickets issued in this epoch.  A thief's ticket
 *            is the pre-increment value returned by its fetch_add.
 *  epoch   : index into the queue's epoch array, or GPUTC_EPOCH_CLOSED.
 *  itasks  : tasks present in the shared portion when the epoch opened.
 *  vtail   : ring index of the first task in the epoch.
 *
 *  Compare libtc/saws_shrb.c:190 (saws_set_stealval) -- same idea, wider
 *  fields.  The CPU layout gives 19 bits to itasks/vtail and 24 to asteals;
 *  we rebalance to 20/20/20 and spend the reclaimed bits on the epoch id.
 */

#define GPUTC_VTAIL_SHIFT      0
#define GPUTC_VTAIL_BITS      20
#define GPUTC_ITASKS_SHIFT    20
#define GPUTC_ITASKS_BITS     20
#define GPUTC_EPOCH_SHIFT     40
#define GPUTC_EPOCH_BITS       4
#define GPUTC_CLAIM_SHIFT     44
#define GPUTC_CLAIM_BITS      20

#define GPUTC_VTAIL_MASK      ((1ULL << GPUTC_VTAIL_BITS)  - 1)
#define GPUTC_ITASKS_MASK     ((1ULL << GPUTC_ITASKS_BITS) - 1)
#define GPUTC_EPOCH_MASK      ((1ULL << GPUTC_EPOCH_BITS)  - 1)
#define GPUTC_CLAIM_MASK      ((1ULL << GPUTC_CLAIM_BITS)  - 1)

/** Added to the steal word to claim one ticket. */
#define GPUTC_CLAIM_INC       (1ULL << GPUTC_CLAIM_SHIFT)

/** Largest ring the 20-bit index fields can address. */
#define GPUTC_MAX_RING        (1 << GPUTC_VTAIL_BITS)


/*==================== EPOCH BOOKKEEPING ====================*/

/** Per-epoch completion state.
 *
 *  The CPU version keeps an ordered array status[SAWS_MAX_STEALS_PER_EPOCH]
 *  and reclaims by scanning for the longest completed prefix.  That works
 *  because geometric halving bounds the thief count at ~22.  On a GPU there
 *  may be thousands of claimants, so we collapse the array to two counters:
 *
 *    retired : how many tickets have finished their transfer
 *    taken   : how many tasks those tickets actually removed
 *
 *  Reclamation is then the O(1) test (closed && retired == ntickets) instead
 *  of an O(maxsteals) scan.  We lose the ability to reclaim a partial prefix
 *  of an open epoch, which the CPU version does; in exchange the epoch state
 *  is 24 bytes instead of ~100 and the test is a single comparison.
 */
struct gpu_epoch_s {
  unsigned int itasks;     /* tasks released into this epoch                 */
  unsigned int vtail;      /* ring index of the first task in the epoch      */
  unsigned int ntickets;   /* tickets outstanding at close (valid if closed) */
  unsigned int retired;    /* tickets that have completed  (device atomic)   */
  unsigned int taken;      /* tasks actually removed       (device atomic)   */
  unsigned int closed;     /* set by the owner when the epoch stops accepting*/
};
typedef struct gpu_epoch_s gpu_epoch_t;


/*==================== THE QUEUE ====================*/

/** Split ring buffer, one per (block, class) pair.
 *
 *  Layout mirrors libtc/saws_shrb.h.  The trailing flexible array is allocated
 *  contiguously by nvshmem_malloc() so that every PE sees the same symmetric
 *  offsets and a remote thief can address q[] directly.
 *
 *  Ownership: 'tail', 'split', 'nlocal' are private to the owning block.  Only
 *  'steal_val' and the epoch counters are touched remotely.
 */
struct gpu_shrb_s {
  /* --- remotely visible, atomically manipulated --- */
  unsigned long long steal_val;              /* the packed word described above */
  gpu_epoch_t        epoch[GPUTC_MAX_EPOCHS];

  /* --- owner-private --- */
  int   tail;          /* first task in the shared portion                    */
  int   split;         /* boundary between shared (below) and local (above)   */
  int   nlocal;        /* tasks in the local-only portion                     */
  int   cur;           /* index of the epoch currently accepting steals       */
  int   last;          /* index of the previous epoch, pending reclamation    */

  /* Block-scoped lock over the head fields above.
   *
   * The CPU version needs nothing here: one worker owns the head and touches
   * it without synchronization.  On the GPU the owner is a whole block, so the
   * warps within it contend.  Because exactly one block owns each queue, this
   * can be a *block*-scoped atomic that stays resident in L1 and never
   * generates cross-SM traffic -- roughly two orders of magnitude cheaper than
   * the global atomics on the steal path, but not free.  This is the main
   * accounting difference from the CPU original for very fine tasks. */
  unsigned int hlock;

  /* --- immutable after create --- */
  int   max_size;      /* ring capacity in tasks                              */
  int   elem_size;     /* bytes per task descriptor                           */
  int   owner_pe;      /* PE this queue lives on                              */
  int   owner_blk;     /* block that owns it                                  */
  int   task_class;    /* class of every task in this queue (see README)      */

  /* --- statistics (device atomics, read on the host after the kernel) --- */
  unsigned long long nsteals;      /* successful steals served                */
  unsigned long long nsteal_fail;  /* steal attempts that found nothing       */
  unsigned long long nreleased;    /* local -> shared transitions             */
  unsigned long long nreclaimed;   /* epochs successfully retired             */

  unsigned char q[];   /* ring storage, elem_size * max_size bytes            */
};
typedef struct gpu_shrb_s gpu_shrb_t;


/*==================== TASK DESCRIPTOR ====================*/

/** Header prepended to every task body.
 *
 *  Deliberately tiny and POD.  The CPU task_t carries a task_class_t, a
 *  created_by, and a priority; we keep class and provenance and drop priority,
 *  which the prototype does not use.  Everything must be trivially copyable
 *  because tasks are moved between devices with raw getmem().
 */
struct gpu_task_s {
  unsigned int  task_class;
  unsigned int  created_by;   /* encoded (pe << 16) | block                   */
  unsigned char body[];
};
typedef struct gpu_task_s gpu_task_t;

/** Device-side task entry point.  Registered per class; see gpu_task.cuh. */
struct gputc_dev_ctx_s;
typedef void (*gputc_task_fn_t)(struct gputc_dev_ctx_s *ctx, void *body);


/*==================== DEVICE CONTEXT ====================*/

/** Everything the persistent kernel needs, passed by value at launch.
 *
 *  Analogous to tc_t + gtc_context_t in the CPU version, flattened so it can
 *  be a kernel parameter rather than a chased pointer.
 */
struct gputc_dev_ctx_s {
  gpu_shrb_t  **queues;      /* [nblocks * nclasses], device array of pointers */
  int           my_pe;
  int           n_pes;
  int           nblocks;
  int           nclasses;
  int           elem_size;
  int           max_size;

  /* termination detection, see gpu_termination.cuh */
  unsigned int      *active_blocks;   /* device-scope count of working blocks  */
  unsigned long long *spawned;        /* device-scope task counters            */
  unsigned long long *completed;
  int               *terminated;      /* set by the host progress engine       */
};
typedef struct gputc_dev_ctx_s gputc_dev_ctx_t;

#endif /* __GPUTC_TYPES_CUH__ */
