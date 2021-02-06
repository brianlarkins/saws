#ifndef __SAWS_SHRB_H__
#define __SAWS_SHRB_H__

#include <sys/types.h>
#include <stdint.h>
#include <shmem.h>
#include <mutex.h>
#include <tc.h>

#define SAWS_MAX_EPOCHS           2L
#define SAWS_MAX_STEALS_PER_EPOCH 22

typedef enum {
  SAWSPopTailTime,
  SAWSPerPopTailTime,
  SAWSGetMetaTime,
  SAWSPerGetMetaTime,
  SAWSProgressTime,
  SAWSPerProgressTime,
  SAWSReclaimTime,
  SAWSPerReclaimTime,
  SAWSEnsureTime,
  SAWSPerEnsureTime,
  SAWSReacquireTime,
  SAWSPerReacquireTime,
  SAWSReleaseTime,
  SAWSPerReleaseTime
} gtc_sdc_gtimestats_e;


typedef enum {
  SAWSGetCalls,
  SAWSNumGets,
  SAWSNumMeta,
  SAWSGetLocalCalls,
  SAWSNumSteals,
  SAWSStealFailsLocked,
  SAWSStealFailsUnlocked,
  SAWSAbortedSteals,
  SAWSProgressCalls,
  SAWSReclaimCalls,
  SAWSEnsureCalls,
  SAWSReacquireCalls,
  SAWSReleaseCalls
} gtc_sdc_gcountstats_e;

struct saws_completion_s {
  uint64_t itasks;                             // initial number of available tasks
  int64_t  vtail;                              // initial tail for this steal epoch
  int      done;                               // true if all outstanding steals are complete
  int      maxsteals;                          // maximum number of steal operations for itasks tasks
  int      status[SAWS_MAX_STEALS_PER_EPOCH];  // ordered completion status for all steals in this epoch
};
typedef struct saws_completion_s saws_completion_t;


struct saws_shrb_s {

  int64_t           tail;      // Index of tail element (between 0 and rb_size-1)
  int64_t           vtail;     // Index of public tail element
  uint64_t          steal_val; // Concatenation of tail, isteals, and asteals
  uint32_t         *targets;   // Holds the last known queue state for all other nodes 
  int64_t           split;     // index of split between local-only and local-shared elements
  int               nlocal;    // Number of elements in the local portion of the queue
  int               nshared;

  synch_mutex_t     lock;      // lock for shared portion of this queue
  int               waiting;   // Am I currently waiting for transactions to complete?

  int               procid;
  int               nproc;
  int               max_size;  // Max size in number of elements
  int               elem_size; // Size of an element in bytes 
  int               reclaimfreq;                        // reclaim dampening frequency
  int               claimed[SAWS_MAX_STEALS_PER_EPOCH]; // # claimed task lookup table
  saws_completion_t completed[SAWS_MAX_EPOCHS];         // completion arrays
  int               cur;                                // index of current completion array
  int               last;                               // index of last completion array

  tc_t             *tc;        // task collection associated with queue (for stats)

  tc_counter_t      nwaited;   // How many times did I have to wait
  tc_counter_t      nreclaimed;// How many times did I reclaim space from the public portion of the queue
  tc_counter_t      nreccalls; // How many times did I even try to reclaim
  tc_counter_t      nrelease;  // Number of times work was released from local->public
  tc_counter_t      nprogress; // Number of otimes we called the progress routine
  tc_counter_t      nreacquire;// Number of times work was reacquired from public->local
  tc_counter_t      ngets;     // Number of times we attempted a steal
  tc_counter_t      nensure;   // Number of times we call reclaim space
  tc_counter_t      nxfer;     // xferred bytes
  tc_counter_t      nsteals;   // number of successful steals
  tc_counter_t      nmeta;     // number of successful steals

  u_int8_t          q[0];      // (shared)  ring buffer data.  This will be allocated
  // contiguous with the rb_s so allocating an rb_s will
  // require "sizeof(struct rb_s) + elem_size*rb_size"
};

typedef struct saws_shrb_s saws_shrb_t;

saws_shrb_t *saws_shrb_create(int elem_size, int max_size, tc_t *tc);
void        saws_shrb_destroy(saws_shrb_t *rb);
void        saws_shrb_reset(saws_shrb_t *rb);

void        saws_shrb_lock(saws_shrb_t *rb, int proc);
void        saws_shrb_unlock(saws_shrb_t *rb, int proc);

int         saws_shrb_head(saws_shrb_t *rb);
int         saws_shrb_local_isempty(saws_shrb_t *rb);
int         saws_shrb_shared_isempty(saws_shrb_t *rb);
int         saws_shrb_local_size(saws_shrb_t *rb);
int         saws_shrb_shared_size(saws_shrb_t *rb);
int         saws_shrb_isempty(saws_shrb_t *rb);
int         saws_shrb_public_size(saws_shrb_t *rb);

void        saws_shrb_release(saws_shrb_t *rb);
void        saws_shrb_release_all(saws_shrb_t *rb);
void        saws_shrb_reacquire(saws_shrb_t *rb);
int         saws_shrb_reclaim_space(saws_shrb_t *rb);

void        saws_shrb_push_head(saws_shrb_t *rb, int proc, void *e, int size);
void        saws_shrb_push_n_head(void *b, int proc, void *e, int n);
void       *saws_shrb_alloc_head(saws_shrb_t *rb);

int         saws_shrb_pop_head(void *b, int proc, void *buf);
int         saws_shrb_pop_tail(saws_shrb_t *rb, int proc, void *buf);
int         saws_shrb_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);
int         saws_shrb_try_pop_n_tail(void *b, int proc, int n, void *buf, int steal_vol);

int         saws_shrb_size(void *b);
int         saws_shrb_full(saws_shrb_t *rb);
int         saws_shrb_empty(saws_shrb_t *rb);

void        saws_shrb_print(saws_shrb_t *rb);

#define saws_shrb_elem_addr(MYRB, PROC, IDX) ((MYRB->q) + (IDX)*(MYRB)->elem_size)
#define saws_shrb_buff_elem_addr(RB, E, IDX) ((u_int8_t*)(E) + (IDX)*(RB)->elem_size)

// ARMCI allocated buffers should be faster/pinned
#define saws_shrb_malloc gtc_shmem_calloc
#define saws_shrb_free   shmem_free

#endif /* __SAWS_SHRB_H__ */
