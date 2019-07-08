#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <mutex.h>

#include "tc.h"
#include "sdc_shr_ring.h"
// #include "tc-internals.h"


/** 
 * Split Deferred-Copy Shared Ring Buffer Semantics:
 * ================================================
 *
 * This queue is split into three parts, as show in the diagram below: a part that is for
 * local access only (allowing for lock-free local interaction with the queue), a part for
 * shared access, and a reserved part that corresponds to remote copies that are in-progress
 * (ie deferred copies).
 *
 * Queue state:
 *
 * Head  - Always points to the element at the head of the local portion.
 *       - The head moves to the RIGHT.  Push and pop are both allowed wrt the head.
 * Tail  - Always points to the next available element at the tail of the shared portion.
 *       - The tail moves to the RIGHT.  Only pop is allowed from the tail.
 * Split - Always points to the element at the tail of the local portion of the queue.
 * Vtail - Virtual tail: points to the element at the tail of the reserved portion of the
 *         queue.  Everything between the head and the vtail is free space.
 * Itail - Tells us the collective progress of all transactions in the reserved portion.
 *         When Itail == tail we can reclaim the reserved space.
 *
 * The ring buffer can in in several states:
 *
 * 1. Empty.  In this case we have nlocal == 0, tail == split, and vtail == itail == tail
 *
 * 2. No Wrap-around:
 *  _______________________________________________________________
 * | |:::|::::::|$$$$$$|/////|                                     |
 * |_|:::|::::::|$$$$$$|/////|_____________________________________|
 *   ^   ^      ^      ^     ^
 * vtail itail tail  split  head
 *
 * : -- Reserved space in the queue (transaction in progress)
 * $ -- Available elements in the shared portion of the queue
 * / -- Reserved elements in the local portion of the queue
 *   -- Free space
 *
 * 2. Wrapped-around:
 *  _______________________________________________________________
 * |/////////|                      |:::::|::::::::::::::|$$$$|////|
 * |/////////|______________________|:::::|::::::::::::::|$$$$|////|
 *           ^                      ^     ^              ^    ^
 *         head                   vtail  itail         tail  split
 */


sdc_shrb_t *sdc_shrb_create(int elem_size, int max_size) {
  sdc_shrb_t  *rb;
  sdc_shrb_t **rbs;
  int procid, nproc;

  setbuf(stdout, NULL);

  procid = shmem_my_pe();
  nproc = shmem_n_pes();

  gtc_lprintf(DBGSHRB, "  Thread %d: sdc_shrb_create()\n", procid);

  // Allocate the struct and the buffer contiguously in shared space
  rbs = malloc(sizeof(sdc_shrb_t*) * nproc);
  assert(rbs != NULL);
  rbs[procid] = shmem_malloc(sizeof(sdc_shrb_t) + elem_size*max_size);

  rb = rbs[procid];

  rb->procid  = procid;
  rb->nproc  = nproc; 
  rb->elem_size = elem_size;
  rb->max_size  = max_size;
  rb->rbs       = rbs;
  sdc_shrb_reset(rb);

  // Initialize the lock
  synch_mutex_init(&rb->lock);

  shmem_barrier_all();

  return rb;
}


void sdc_shrb_reset(sdc_shrb_t *rb) {
  // Reset state to empty
  rb->nlocal = 0;
  rb->tail   = 0;
  rb->itail  = 0;
  rb->vtail  = 0;
  rb->split  = 0;

  rb->waiting= 0;

  // Reset queue statistics
  rb->nrelease   = 0;
  rb->nreacquire = 0;
  rb->nwaited    = 0;
  rb->nreclaimed = 0;
}


void sdc_shrb_destroy(sdc_shrb_t *rb) {
  free(rb->rbs);
  shmem_free(rb);
}

void sdc_shrb_print(sdc_shrb_t *rb) {
  printf("rb: %p {\n", rb);
  printf("   procid  = %d\n", rb->procid);
  printf("   nproc  = %d\n", rb->nproc);
  printf("   nlocal    = %d\n", rb->nlocal);
  printf("   head      = %d\n", sdc_shrb_head(rb));
  printf("   split     = %d\n", rb->split);
  printf("   tail      = %d\n", rb->tail);
  printf("   itail     = %d\n", rb->itail);
  printf("   vtail     = %d\n", rb->vtail);
  printf("   max_size  = %d\n", rb->max_size);
  printf("   elem_size = %d\n", rb->elem_size);
  printf("   local_size = %d\n", sdc_shrb_local_size(rb));
  printf("   shared_size= %d\n", sdc_shrb_shared_size(rb));
  printf("   public_size= %d\n", sdc_shrb_public_size(rb));
  printf("   size       = %d\n", sdc_shrb_size(rb));
  printf("}\n");
}

/*==================== STATE QUERIES ====================*/


int sdc_shrb_head(sdc_shrb_t *rb) {
  return (rb->split + rb->nlocal - 1) % rb->max_size;
}


int sdc_shrb_local_isempty(sdc_shrb_t *rb) {
  return rb->nlocal == 0;
}


int sdc_shrb_shared_isempty(sdc_shrb_t *rb) {
  return rb->tail == rb->split;
}


int sdc_shrb_isempty(sdc_shrb_t *rb) {
  return sdc_shrb_local_isempty(rb) && sdc_shrb_shared_isempty(rb);
}


int sdc_shrb_local_size(sdc_shrb_t *rb) {
  return rb->nlocal;
}


int sdc_shrb_shared_size(sdc_shrb_t *rb) {
  if (sdc_shrb_shared_isempty(rb)) // Shared is empty
    return 0;
  else if (rb->tail < rb->split)   // No wrap-around
    return rb->split - rb->tail;
  else                             // Wrap-around
    return rb->split + rb->max_size - rb->tail;
}


int sdc_shrb_public_size(sdc_shrb_t *rb) {
  if (rb->vtail == rb->split) {    // Public is empty
    assert (rb->tail == rb->itail && rb->tail == rb->split);
    return 0;
  }
  else if (rb->vtail < rb->split)  // No wrap-around
    return rb->split - rb->vtail;
  else                             // Wrap-around
    return rb->split + rb->max_size - rb->vtail;
}


int sdc_shrb_size(sdc_shrb_t *rb) {
  return sdc_shrb_local_size(rb) + sdc_shrb_shared_size(rb);
}


/*==================== SYNCHRONIZATION ====================*/


void sdc_shrb_lock(synch_mutex_t *lock, int proc) {
  synch_mutex_lock(lock, proc);
}


int sdc_shrb_trylock(synch_mutex_t *lock, int proc) {
  return synch_mutex_trylock(lock, proc);
}


void sdc_shrb_unlock(synch_mutex_t *lock, int proc) {
  synch_mutex_unlock(lock, proc);
}


/*==================== SPLIT MOVEMENT ====================*/


int sdc_shrb_reclaim_space(sdc_shrb_t *rb) {
  int reclaimed = 0;
  int vtail = rb->vtail;
  int itail = rb->itail; // Capture these values since we are doing this
  int tail  = rb->tail;  // without a lock
  
  if (vtail != tail && itail == tail) {
    rb->vtail = tail;
    if (tail > vtail)
      reclaimed = tail - vtail;
    else
      reclaimed = rb->max_size - vtail + tail;

    assert(reclaimed > 0);
    rb->nreclaimed++;
  }

  return reclaimed;
}
  

void sdc_shrb_ensure_space(sdc_shrb_t *rb, int n) {
  // Ensure that there is enough free space in the queue.  If there isn't
  // wait until others finish their deferred copies so we can reclaim space.
  if (rb->max_size - (sdc_shrb_local_size(rb) + sdc_shrb_public_size(rb)) < n) {
    sdc_shrb_lock(&rb->lock, rb->procid);
    {
      if (rb->max_size - sdc_shrb_size(rb) < n) {
        // Error: amount of reclaimable space is less than what we need.
        // Try increasing the size of the queue.
        printf("SDC_SHRB: Error, not enough space in the queue to push %d elements\n", n);
        sdc_shrb_print(rb);
        assert(0);
      }
      rb->waiting = 1;
      while (sdc_shrb_reclaim_space(rb) == 0) /* Busy Wait */ ;
      rb->waiting = 0;
      rb->nwaited++;
    }
    sdc_shrb_unlock(&rb->lock, rb->procid);
  }
}

      
void sdc_shrb_release(sdc_shrb_t *rb) {
  // Favor placing work in the shared portion -- if there is only one task
  // available this scheme will put it in the shared portion.
  if (sdc_shrb_local_size(rb) > 0 && sdc_shrb_shared_size(rb) == 0) {
    int amount  = sdc_shrb_local_size(rb)/2 + sdc_shrb_local_size(rb) % 2;
    rb->nlocal -= amount;
    rb->split   = (rb->split + amount) % rb->max_size;
    rb->nrelease++;
  }
}


void sdc_shrb_release_all(sdc_shrb_t *rb) {
  int amount  = sdc_shrb_local_size(rb);
  rb->nlocal -= amount;
  rb->split   = (rb->split + amount) % rb->max_size;
  rb->nrelease++;
}


int sdc_shrb_reacquire(sdc_shrb_t *rb) {
  int amount = 0;

  // Favor placing work in the local portion -- if there is only one task
  // available this scheme will put it in the local portion.
  sdc_shrb_lock(&rb->lock, rb->procid);
  {
    if (sdc_shrb_shared_size(rb) > sdc_shrb_local_size(rb)) {
      int diff    = sdc_shrb_shared_size(rb) - sdc_shrb_local_size(rb);
      amount      = diff/2 + diff % 2;
      rb->nlocal += amount;
      rb->split   = (rb->split - amount);
      if (rb->split < 0)
        rb->split += rb->max_size;
      rb->nreacquire++;
    }

    // Assertion: sdc_shrb_local_isempty(rb) => sdc_shrb_isempty(rb)
    assert(!sdc_shrb_local_isempty(rb) || (sdc_shrb_isempty(rb) && sdc_shrb_local_isempty(rb)));
  }
  sdc_shrb_unlock(&rb->lock, rb->procid);

  return amount;
}


/*==================== PUSH OPERATIONS ====================*/


static inline void sdc_shrb_push_n_head_impl(sdc_shrb_t *rb, int proc, void *e, int n, int size) {
  int head, old_head;

  assert(size <= rb->elem_size);
  assert(size == rb->elem_size || n == 1);  // n > 1 ==> size == rb->elem_size
  assert(proc == rb->procid);

  // Make sure there is enough space for n elements
  sdc_shrb_ensure_space(rb, n);
  
  // Proceed with the push
  old_head    = sdc_shrb_head(rb);
  rb->nlocal += n;
  head        = sdc_shrb_head(rb);

  if (head > old_head || old_head == rb->max_size - 1) {
    memcpy(sdc_shrb_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, n*size);
  }

  // This push wraps around, break it into two parts
  else {
    int part_size = rb->max_size - 1 - old_head;

    memcpy(sdc_shrb_elem_addr(rb, proc, old_head+1), e, part_size*size);
    memcpy(sdc_shrb_elem_addr(rb, proc, 0), sdc_shrb_buff_elem_addr(rb, e, part_size), (n - part_size)*size);
  }
}

void sdc_shrb_push_head(sdc_shrb_t *rb, int proc, void *e, int size) {
  // sdc_shrb_push_n_head_impl(rb, proc, e, 1, size);
  int old_head;

  assert(size <= rb->elem_size);
  assert(proc == rb->procid);

  // Make sure there is enough space for n elements
  sdc_shrb_ensure_space(rb, 1);
  
  // Proceed with the push
  old_head    = sdc_shrb_head(rb);
  rb->nlocal += 1;

  memcpy(sdc_shrb_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, size);

  // printf("(%d) pushed head\n", rb->procid);
}

void sdc_shrb_push_n_head(sdc_shrb_t *rb, int proc, void *e, int n) {
  sdc_shrb_push_n_head_impl(rb, proc, e, n, rb->elem_size);
}


void *sdc_shrb_alloc_head(sdc_shrb_t *rb) {
  // Make sure there is enough space for 1 element
  sdc_shrb_ensure_space(rb, 1);
  
  rb->nlocal += 1;
  
  return sdc_shrb_elem_addr(rb, rb->procid, sdc_shrb_head(rb));
}

/*==================== POP OPERATIONS ====================*/


int sdc_shrb_pop_head(sdc_shrb_t *rb, int proc, void *buf) {
  int   old_head;
  int   buf_valid = 0;

  assert(proc == rb->procid);

  // If we are out of local work, try to reacquire
  if (sdc_shrb_local_isempty(rb))
    sdc_shrb_reacquire(rb);

  if (sdc_shrb_local_size(rb) > 0) {
    old_head = sdc_shrb_head(rb);

    memcpy(buf, sdc_shrb_elem_addr(rb, proc, old_head), rb->elem_size);

    rb->nlocal--;
    buf_valid = 1;
  }

  // Assertion: !buf_valid => sdc_shrb_isempty(rb)
  assert(buf_valid || (!buf_valid && sdc_shrb_isempty(rb)));
  
  // printf("(%d) popped head\n", rb->procid);

  return buf_valid;
}


int sdc_shrb_pop_tail(sdc_shrb_t *rb, int proc, void *buf) {
  return sdc_shrb_pop_n_tail(rb, proc, 1, buf, STEAL_CHUNK);
}

/* Pop up to N elements off the tail of the queue, putting the result into the user-
 *  supplied buffer.
 *
 *  @param myrb  Pointer to the RB
 *  @param proc  Process to perform the pop on
 *  @param n     Requested/Max. number of elements to pop.
 *  @param e     Buffer to store result in.  Should be rb->elem_size*n bytes big and
 *               should also be allocated with sdc_shrb_malloc().
 *  @param steal_vol Enumeration that selects between different schemes for determining
 *               the amount we steal.
 *  @param trylock Indicates whether to use trylock or lock.  Using trylock will result
 *               in a fail return value when trylock does not succeed.
 *
 *  @return      The number of tasks stolen or -1 on failure
 */

static inline int sdc_shrb_pop_n_tail_impl(sdc_shrb_t *myrb, int proc, int n, void *e, int steal_vol, int trylock) {
  // sdc_shrb_t *trb = malloc(sizeof(sdc_shrb_t));

  sdc_shrb_t trb;

  // assert(trb != NULL);
  
  // Attempt to get the lock
  if (trylock) {
    if (!sdc_shrb_trylock(&myrb->lock, proc)) {
      return -1;
    }
  } else {
     sdc_shrb_lock(&myrb->lock, proc);
  }

  // Copy the remote RB's metadata
  // ARMCI_Get(myrb->rbs[proc], trb, sizeof(sdc_shrb_t), proc);
  shmem_getmem(&trb, myrb, sizeof(sdc_shrb_t), proc);

  switch (steal_vol) {
    case STEAL_HALF:
      n = MIN(sdc_shrb_shared_size(&trb)/2 + sdc_shrb_shared_size(&trb) % 2, n);
      break;
    case STEAL_ALL:
      n = MIN(sdc_shrb_shared_size(&trb), n);
      break;
    case STEAL_CHUNK:
      // Get as much as possible up to N.
      n = MIN(n, sdc_shrb_shared_size(&trb));
      break;
    default:
      printf("Error: Unknown steal volume heuristic.\n");
      assert(0);
  }

  // Reserve N elements by advancing the victim's tail
  if (n > 0) {
    int  new_tail;
    // int  metadata;
    int  xfer_size;
    int *loc_addr, *rem_addr;
    
    new_tail    = ((&trb)->tail + n) % (&trb)->max_size;
    // metadata[0] = new_tail; // itail field
    // metadata[1] = new_tail; // tail field in rb struct

    loc_addr    = &new_tail;
    rem_addr    = &myrb->tail;
    xfer_size   = 1*sizeof(int);
    // ARMCI_Put(loc_addr, rem_addr, xfer_size, proc);
    shmem_putmem(rem_addr, loc_addr, xfer_size, proc);

    sdc_shrb_unlock(&myrb->lock, proc); // Deferred copy unlocks early
    
    // Transfer work into the local buffer
    if ((&trb)->tail + (n-1) < (&trb)->max_size) {    // No need to wrap around
   
      shmem_getmem_nbi(e, sdc_shrb_elem_addr(myrb, proc, (&trb)->tail), n * (&trb)->elem_size, proc);    // Store n elems, starting at remote tail, in e
      shmem_quiet();
    
    } else {    // Need to wrap around
      int part_size  = (&trb)->max_size - (&trb)->tail;
     
      shmem_getmem_nbi(sdc_shrb_buff_elem_addr(&trb, e, 0), sdc_shrb_elem_addr(myrb, proc, (&trb)->tail), part_size * (&trb)->elem_size, proc);
      
      shmem_getmem_nbi(sdc_shrb_buff_elem_addr(&trb, e, part_size), sdc_shrb_elem_addr(myrb, proc, 0), (n - part_size) * (&trb)->elem_size, proc);

      shmem_quiet();

      shmem_quiet();
    }

#ifndef SDC_NODC
    // Accumulate itail_inc onto the victim's intermediate tail
    {
      int itail_inc;
      long int stride = 1;
      int count = sizeof(int);

      // How much should we add to the itail?  If we caused a wraparound, we need to also wrap itail.
      if (new_tail > (&trb)->tail)
        itail_inc = n;
      else
        itail_inc = n - (&trb)->max_size;
      // printf("n: %d, itail_inc: %d, max_size: %d\n", n, itail_inc, (&trb)->max_size);
      // err = ARMCI_AccS(ARMCI_ACC_INT, &scale, /* src */ &itail_inc, &stride, 
      //                 /* dst */ &myrb->rbs[proc]->itail, &stride, &count, 0, proc);
      // shmem_iput(&(myrb->itail), &itail_inc, stride, stride, count, proc);

      int oldval;
      // err = ARMCI_Rmw(ARMCI_FETCH_AND_ADD, &oldval, &myrb->rbs[proc]->itail, itail_inc, proc);
      oldval = shmem_atomic_fetch_add(&(myrb->itail), itail_inc, proc);
      
      // int newval = (&trb)->itail + itail_inc;
      // err = ARMCI_Put(&newval, &myrb->rbs[proc]->itail, sizeof(int), proc);
      // shmem_putmem(&(myrb->itail), &newval, sizeof(int), proc);
      shmem_quiet();
    }
#else
    shmem_quiet();    
    sdc_shrb_unlock(&myrb->lock, proc);
#endif

    } else /* (n <= 0) */ {
       sdc_shrb_unlock(&myrb->lock, proc);
    }
  
    return n;
}

int sdc_shrb_pop_n_tail(sdc_shrb_t *myrb, int proc, int n, void *e, int steal_vol) {
  return sdc_shrb_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 0);
}
  
int sdc_shrb_try_pop_n_tail(sdc_shrb_t *myrb, int proc, int n, void *e, int steal_vol) {
  return sdc_shrb_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 1);
}


