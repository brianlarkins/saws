#ifndef __SAWS_SHRB_H__
#define __SAWS_SHRB_H__

#include <sys/types.h>
#include <stdint.h>
#include <shmem.h>
#include <mutex.h>
#include <tc.h>

struct saws_shrb_s {
  int             itail;     // Index of the intermediate tail (between vtail and tail)
  long            tail;      // Index of tail element (between 0 and rb_size-1)
  int             completed; // Number of completed steals
  uint64_t        steal_val; // Concatenation of tail, isteals, and asteals

  int             nlocal;    // Number of elements in the local portion of the queue
  int             vtail;     // Index of the virtual tail
  int             split;     // index of split between local-only and local-shared elements

  synch_mutex_t   lock;      // lock for shared portion of this queue
  int             waiting;   // Am I currently waiting for transactions to complete?
  
  int             procid;
  int             nproc;
  int             max_size;  // Max size in number of elements
  int             elem_size; // Size of an element in bytes

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

  struct saws_shrb_s **rbs;   // (private) array of base addrs for all rbs
  u_int8_t        q[0];      // (shared)  ring buffer data.  This will be allocated
                             // contiguous with the rb_s so allocating an rb_s will
                             // require "sizeof(struct rb_s) + elem_size*rb_size"
};

typedef struct saws_shrb_s saws_shrb_t;

saws_shrb_t *saws_shrb_create(int elem_size, int max_size);
void        saws_shrb_destroy(saws_shrb_t *rb);
void        saws_shrb_reset(saws_shrb_t *rb);

void        saws_shrb_lock(saws_shrb_t *rb, int proc);
void        saws_shrb_unlock(saws_shrb_t *rb, int proc);

int         saws_shrb_head(saws_shrb_t *rb);
int         saws_shrb_local_isempty(saws_shrb_t *rb);
int         saws_shrb_shared_isempty(saws_shrb_t *rb);
int         saws_shrb_local_size(saws_shrb_t *rb);
int         saws_shrb_shared_size(saws_shrb_t *rb);
int         saws_shrb_reserved_size(saws_shrb_t *rb);
int         saws_shrb_public_size(saws_shrb_t *rb);

void        saws_shrb_release(saws_shrb_t *rb);
void        saws_shrb_release_all(saws_shrb_t *rb);
void        saws_shrb_reacquire(saws_shrb_t *rb);
void        saws_shrb_reclaim_space(saws_shrb_t *rb);

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

#define saws_shrb_elem_addr(MYRB, PROC, IDX) ((MYRB)->q + (IDX)*(MYRB)->elem_size)
#define saws_shrb_buff_elem_addr(RB, E, IDX) ((u_int8_t*)(E) + (IDX)*(RB)->elem_size)

// ARMCI allocated buffers should be faster/pinned
#define saws_shrb_malloc shmem_calloc
#define saws_shrb_free   shmem_free

#endif /* __SAWS_SHRB_H__ */