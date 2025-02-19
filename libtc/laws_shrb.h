#ifndef __SDC_SHR_RING_H__
#define __SDC_SHR_RING_H__

#include <mutex.h>
#include <shmem.h>
#include <sys/types.h>
#include <tc.h>

typedef enum {
  LAWSPopTailTime,
  LAWSPerPopTailTime,
  LAWSStealTime,
  LAWSPerStealTime,
  LAWSGetMetaTime,
  LAWSPerGetMetaTime,
  LAWSProgressTime,
  LAWSPerProgressTime,
  LAWSReclaimTime,
  LAWSPerReclaimTime,
  LAWSEnsureTime,
  LAWSPerEnsureTime,
  LAWSReacquireTime,
  LAWSPerReacquireTime,
  LAWSReleaseTime,
  LAWSPerReleaseTime,
  LAWSGlobalRetTime,
  LAWSPerGlobalRetTime
} gtc_sdc_gtimestats_e;

typedef enum {
  LAWSGetCalls,
  LAWSNumGets,
  LAWSNumMeta,
  LAWSGetLocalCalls,
  LAWSNumSteals,
  LAWSNumLocalSteals,
  LAWSNumGlobalSteals,
  LAWSStealFailsLocked,
  LAWSStealFailsUnlocked,
  LAWSAbortedSteals,
  LAWSProgressCalls,
  LAWSReclaimCalls,
  LAWSEnsureCalls,
  LAWSReacquireCalls,
  LAWSReleaseCalls,
  LAWSGlobalRetCalls,
  LAWSNumTasksStolen,
  LAWSNumTasksStolenGlobally,
  LAWSNumTasksStolenLocally
} gtc_sdc_gcountstats_e;

struct laws_s {
  int itail; // Index of the intermediate tail (between vtail and tail)
  int tail;  // Index of tail element (between 0 and rb_size-1)

  int nlocal; // Number of elements in the local portion of the queue
  int vtail;  // Index of the virtual tail
  int split;  // index of split between local-only and local-shared elements

  synch_mutex_t lock; // lock for shared portion of this queue
  int waiting;        // Am I currently waiting for transactions to complete?

  int procid;
  int nproc;     // number of processes in total (aka. across all nodes)
  int max_size;  // Max size in number of elements
  int elem_size; // Size of an element in bytes
  int *avg_local_tasks_stolen;
  int *num_local_steals;
  int root;   // the root process relative to this one
  int ncores; // number of cores on each node
  int rank;
  int has_work; // only attempt to retrieve work locally if this has been set
                // (aka. we have previously successfully retrieved work through
                // random selection)
  // uint8_t         *gaddrs; // the addresses of the global metadata stored on
  // the root process uint8_t         *global; // our copy of the global
  // metadata
  uint64_t
      *global_bits; // bitfield indicating work status of each process on a node
  uint64_t gb_copy; // where the copy is stored when retrieved from memory
  uint8_t *has_work_avail;
  // uint8_t         *g_meta; // pointer to our process's metadata specifically
  // uint8_t         *gaddr;  // same as above, but with reference to the
  // address from which that data is pulled
  uint64_t our_bits;   // used when modifying global bitfield
  uint64_t our_invert; // our_bits but inverted

  tc_t *tc; // task collection associated with queue (for stats)

  tc_counter_t nwaited;    // How many times did I have to wait
  tc_counter_t nreclaimed; // How many times did I reclaim space from the public
                           // portion of the queue
  tc_counter_t nreccalls;  // How many times did I even try to reclaim
  tc_counter_t nrelease; // Number of times work was released from local->public
  tc_counter_t nprogress; // Number of otimes we called the progress routine
  tc_counter_t
      nreacquire;     // Number of times work was reacquired from public->local
  tc_counter_t ngets; // Number of times we attempted a steal
  tc_counter_t nensure; // Number of times we call reclaim space
  tc_counter_t nxfer;   // xferred bytes
  tc_counter_t nsteals; // number of successful steals
  tc_counter_t nmeta;   // number of successful steals
  tc_counter_t ngret;   // number of times global array is retrieved

  struct laws_s **rbs; // (private) array of base addrs for all rbs
  u_int8_t q[0];       // (shared)  ring buffer data.  This will be allocated
                       // contiguous with the rb_s so allocating an rb_s will
                       // require "sizeof(struct rb_s) + elem_size*rb_size"
};

typedef struct laws_s laws_t;

typedef uint8_t laws_global_t;

laws_t *laws_create(int elem_size, int max_size, tc_t *tc);
void laws_destroy(laws_t *rb);
void laws_reset(laws_t *rb);

void laws_lock(laws_t *rb, int proc);
void laws_unlock(laws_t *rb, int proc);

int laws_head(laws_t *rb);
int laws_local_isempty(laws_t *rb);
int laws_shared_isempty(laws_t *rb);
int laws_local_size(laws_t *rb);
int laws_shared_size(laws_t *rb);
int laws_reserved_size(laws_t *rb);
int laws_public_size(laws_t *rb);

void laws_release(laws_t *rb);
void laws_release_all(laws_t *rb);
int laws_reacquire(laws_t *rb);
int laws_reclaim_space(laws_t *rb);

void laws_push_head(laws_t *rb, int proc, void *e, int size);
void laws_push_n_head(void *b, int proc, void *e, int n);
void *laws_alloc_head(laws_t *rb);

int laws_pop_head(void *b, int proc, void *buf);
int laws_pop_tail(laws_t *rb, int proc, void *buf);
int laws_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);
int laws_try_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);

int laws_size(void *b);
int laws_full(laws_t *rb);
int laws_empty(laws_t *rb);

void laws_print(laws_t *rb);

#define laws_elem_addr(MYRB, PROC, IDX) ((MYRB)->q + (IDX) * (MYRB)->elem_size)
#define laws_buff_elem_addr(RB, E, IDX)                                        \
  ((u_int8_t *)(E) + (IDX) * (RB)->elem_size)

// ARMCI allocated buffers should be faster/pinned
#define laws_malloc gtc_shmem_calloc
#define laws_free shmem_free

#endif /* __SDC_SHR_RING_H__ */
