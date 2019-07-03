#ifndef __SDC_SHR_RING_H__
#define __SDC_SHR_RING_H__

#include <sys/types.h>
#include <shmem.h>
#include <mutex.h>
#include <tc.h>

struct sdc_shrb_s {
  int             itail;     // Index of the intermediate tail (between vtail and tail)
  int             tail;      // Index of tail element (between 0 and rb_size-1)

  int             nlocal;    // Number of elements in the local portion of the queue
  int             vtail;     // Index of the virtual tail
  int             split;     // index of split between local-only and local-shared elements

  synch_mutex_t   lock;      // lock for shared portion of this queue
  int             waiting;   // Am I currently waiting for transactions to complete?
  
  int             procid;
  int             nproc;
  int             max_size;  // Max size in number of elements
  int             elem_size; // Size of an element in bytes
 
  unsigned long   nwaited;   // How many times did I have to wait
  unsigned long   nreclaimed;// How many times did I reclaim space from the public portion of the queue
  unsigned long   nrelease;  // Number of times work was released from local->public
  unsigned long   nreacquire;// Number of times work was reacquired from public->local

  struct sdc_shrb_s **rbs;   // (private) array of base addrs for all rbs
  u_int8_t        q[0];      // (shared)  ring buffer data.  This will be allocated
                             // contiguous with the rb_s so allocating an rb_s will
                             // require "sizeof(struct rb_s) + elem_size*rb_size"
};

typedef struct sdc_shrb_s sdc_shrb_t;

sdc_shrb_t *sdc_shrb_create(int elem_size, int max_size);
void        sdc_shrb_destroy(sdc_shrb_t *rb);
void        sdc_shrb_reset(sdc_shrb_t *rb);

void        sdc_shrb_lock(synch_mutex_t *lock, int proc);
void        sdc_shrb_unlock(synch_mutex_t *lock, int proc);

int         sdc_shrb_head(sdc_shrb_t *rb);
int         sdc_shrb_local_isempty(sdc_shrb_t *rb);
int         sdc_shrb_shared_isempty(sdc_shrb_t *rb);
int         sdc_shrb_local_size(sdc_shrb_t *rb);
int         sdc_shrb_shared_size(sdc_shrb_t *rb);
int         sdc_shrb_reserved_size(sdc_shrb_t *rb);
int         sdc_shrb_public_size(sdc_shrb_t *rb);

void        sdc_shrb_release(sdc_shrb_t *rb);
void        sdc_shrb_release_all(sdc_shrb_t *rb);
int         sdc_shrb_reacquire(sdc_shrb_t *rb);
int         sdc_shrb_reclaim_space(sdc_shrb_t *rb);

void        sdc_shrb_push_head(sdc_shrb_t *rb, int proc, void *e, int size);
void        sdc_shrb_push_n_head(sdc_shrb_t *rb, int proc, void *e, int n);
void       *sdc_shrb_alloc_head(sdc_shrb_t *rb);

int         sdc_shrb_pop_head(sdc_shrb_t *rb, int proc, void *buf);
int         sdc_shrb_pop_tail(sdc_shrb_t *rb, int proc, void *buf);
int         sdc_shrb_pop_n_tail(sdc_shrb_t *rb, int proc, int n, void *buf, int steal_vol);
int         sdc_shrb_try_pop_n_tail(sdc_shrb_t *rb, int proc, int n, void *buf, int steal_vol);

int         sdc_shrb_size(sdc_shrb_t *rb);
int         sdc_shrb_full(sdc_shrb_t *rb);
int         sdc_shrb_empty(sdc_shrb_t *rb);

void        sdc_shrb_print(sdc_shrb_t *rb);

#define sdc_shrb_elem_addr(MYRB, PROC, IDX) ((MYRB)->rbs[PROC]->q + (IDX)*(MYRB)->elem_size)
#define sdc_shrb_buff_elem_addr(RB, E, IDX) ((u_int8_t*)(E) + (IDX)*(RB)->elem_size)

// ARMCI allocated buffers should be faster/pinned
#define sdc_shrb_malloc shmem_calloc
#define sdc_shrb_free   shmem_free

#endif /* __SDC_SHR_RING_H__ */
