#ifndef __LAWS_SHRB_H__
#define __LAWS_SHRB_H__

#include <sys/types.h>
#include <shmem.h>
#include <mutex.h>
#include <tc.h>

typedef enum {
  LAWSPopTailTime,
  LAWSPerPopTailTime,
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
  LAWSPerReleaseTime
} gtc_laws_gtimestats_e;


typedef enum {
  LAWSGetCalls,
  LAWSNumGets,
  LAWSNumMeta,
  LAWSGetLocalCalls,
  LAWSNumSteals,
  LAWSStealFailsLocked,
  LAWSStealFailsUnlocked,
  LAWSAbortedSteals,
  LAWSProgressCalls,
  LAWSReclaimCalls,
  LAWSEnsureCalls,
  LAWSReacquireCalls,
  LAWSReleaseCalls
} gtc_laws_gcountstats_e;


struct laws_local_s {
  int             vtail;     // out-of-date version of the virtual tail (functions similarly to itail in SDC; 
                             // keeps track of which steals have completed)

  int             nlocal;    // Number of elements in the local portion of the queue
  int             head;      // used along with nlocal to determine split position   

  synch_mutex_t   lock;      // lock for shared portion of this queue
  int             waiting;   // Am I currently waiting for transactions to complete?
  
  int             procid;
  int             nproc;
  int             ncores;
  int             root;      // root process relative to this rank
  int             rank_in_node;
  int             max_size;  // Max size in number of elements
  int             elem_size; // Size of an element in bytes
  struct laws_global_s   *global;
  struct laws_global_s   *g_meta;

  tc_t           *tc;        // task collection associated with queue (for stats)

  tc_counter_t    nwaited;   // How many times did I have to wait
  tc_counter_t    nreclaimed;// How many times did I reclaim space from the public portion of the queue
  tc_counter_t    nreccalls; // How many times did I even try to reclaim
  tc_counter_t    nrelease;  // Number of times work was released from local->public
  tc_counter_t    nprogress; // Number of otimes we called the progress routine
  tc_counter_t    nreacquire;// Number of times work was reacquired from public->local
  tc_counter_t    ngets;     // Number of times we attempted a steal
  tc_counter_t    nensure;   // Number of times we call reclaim space
  tc_counter_t    nxfer;     // xferred bytes
  tc_counter_t    nsteals;   // number of successful steals
  tc_counter_t    nmeta;     // number of successful steals

  struct laws_s **rbs;   // (private) array of base addrs for all rbs
  u_int8_t        q[0];      // (shared)  ring buffer data.  This will be allocated
                             // contiguous with the rb_s so allocating an rb_s will
                             // require "sizeof(struct rb_s) + elem_size*rb_size"
};

struct laws_global_s {
    int                 max_size;
    int                 elem_size;
    int                 vtail;     // Index of the virtual tail
    int                 split;     // index of split between local-only and local-shared elements
    int                 tail;      // Index of tail element (between 0 and rb_size-1)
    struct laws_local_s *local;    // pointer to local metadata and queue
    synch_mutex_t       lock;
};

typedef struct laws_local_s laws_local_t;

typedef struct laws_global_s laws_global_t;

laws_local_t *laws_create(int elem_size, int max_size, tc_t *tc);
void        laws_destroy(laws_local_t *rb);
void        laws_reset(laws_local_t *rb);

void        laws_lock(laws_global_t *rb, int proc);
void        laws_unlock(laws_global_t *rb, int proc);

int         laws_head(laws_local_t *rb);
int         laws_local_isempty(laws_local_t *rb);
int         laws_shared_isempty(laws_global_t *rb);
int         laws_local_size(laws_local_t *rb);
int         laws_shared_size(laws_global_t *rb);
int         laws_reserved_size(laws_local_t *rb);
int         laws_public_size(laws_global_t *rb);

void        laws_release(laws_local_t *rb);
void        laws_release_all(laws_local_t *rb);
int         laws_reacquire(laws_local_t *rb);
int         laws_reclaim_space(laws_local_t *rb);

void        laws_push_head(laws_local_t *rb, int proc, void *e, int size);
void        laws_push_n_head(void *b, int proc, void *e, int n);
void       *laws_alloc_head(laws_local_t *rb);

int         laws_pop_head(void *b, int proc, void *buf);
int         laws_pop_tail(laws_local_t *rb, int proc, void *buf);
int         laws_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);
int         laws_try_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);

int         laws_size(void *b);
int         laws_full(laws_local_t *rb);
int         laws_empty(laws_local_t *rb);

void        laws_print(laws_local_t *rb);

#define laws_elem_addr(MYRB, PROC, IDX) ((MYRB)->q + (IDX)*(MYRB)->elem_size)
#define laws_buff_elem_addr(RB, E, IDX) ((u_int8_t*)(E) + (IDX)*(RB)->elem_size)

// ARMCI allocated buffers should be faster/pinned
#define laws_malloc gtc_shmem_calloc
#define laws_free   shmem_free

#endif /* __LAWS_SHR_RING_H__ */
