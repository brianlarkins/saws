/*********************************************************************/
/*                                                                   */
/*  laws_shrb.c - scioto lock-based  ring buffer q implementation */
/*    (c) 2020 see COPYRIGHT in top-level                            */
/*                                                                   */
/*********************************************************************/
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <mutex.h>

#include "tc.h"
#include "laws_shrb.h"


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


laws_t *laws_create(int elem_size, int max_size, tc_t *tc) {
  GTC_ENTRY();
  laws_t  *rb;
  int procid, nproc;

  setbuf(stdout, NULL);

  procid = shmem_my_pe();
  nproc = shmem_n_pes();

  gtc_lprintf(DBGSHRB, "  Thread %d: laws_create()\n", procid);

  // Allocate the struct and the buffer contiguously in shared space
  rb = gtc_shmem_malloc(sizeof(laws_t) + elem_size*max_size);

  // initialize global arrays
  // need another array to avoid undefined behavior
  int cores_per_node = sysconf(_SC_NPROCESSORS_ONLN);
  rb->gaddrs = gtc_shmem_calloc(cores_per_node, sizeof(laws_global_t));
  rb->global = calloc(cores_per_node, sizeof(laws_global_t));
  
  // set pointers specifically for this process
  rb->ncores = cores_per_node;
  rb->rank = procid % rb->ncores;
  rb->root = procid - rb->rank;
  rb->g_meta = &rb->global[rb->rank];
  rb->gaddr = &rb->gaddrs[rb->rank];

  rb->procid  = procid;
  rb->nproc  = nproc;
  rb->elem_size = elem_size;
  rb->max_size  = max_size;
  laws_reset(rb);

  rb->tc = tc;

  // Initialize the lock
  synch_mutex_init(&rb->lock);

  shmem_barrier_all();

  GTC_EXIT(rb);
}


void laws_reset(laws_t *rb) {
  GTC_ENTRY();
  // Reset state to empty
  rb->nlocal = 0;
  rb->tail   = 0;
  rb->itail  = 0;
  rb->vtail  = 0;
  rb->split  = 0;

  // set default values for global md; save to root
  *(rb->g_meta) = 0;
  shmem_putmem(rb->gaddr, rb->g_meta, sizeof(laws_global_t), rb->root);

  rb->waiting= 0;

  // Reset queue statistics
  rb->nrelease   = 0;
  rb->nreacquire = 0;
  rb->nwaited    = 0;
  rb->nreclaimed = 0;
  GTC_EXIT();
}


void laws_destroy(laws_t *rb) {
  GTC_ENTRY();
  shmem_free(rb);
  GTC_EXIT();
}

void laws_print(laws_t *rb) {
  GTC_ENTRY();
  printf("rb: %p {\n", rb);
  printf("   procid  = %d\n", rb->procid);
  printf("   nproc  = %d\n", rb->nproc);
  printf("   nlocal    = %d\n", rb->nlocal);
  printf("   head      = %d\n", laws_head(rb));
  printf("   split     = %d\n", rb->split);
  printf("   tail      = %d\n", rb->tail);
  printf("   itail     = %d\n", rb->itail);
  printf("   vtail     = %d\n", rb->vtail);
  printf("   max_size  = %d\n", rb->max_size);
  printf("   elem_size = %d\n", rb->elem_size);
  printf("   local_size = %d\n", laws_local_size(rb));
  printf("   shared_size= %d\n", laws_shared_size(rb));
  printf("   public_size= %d\n", laws_public_size(rb));
  printf("   size       = %d\n", laws_size(rb));
  printf("}\n");
  GTC_EXIT();
}

/*==================== STATE QUERIES ====================*/


int laws_head(laws_t *rb) {
  return (rb->split + rb->nlocal - 1) % rb->max_size;
}


int laws_local_isempty(laws_t *rb) {
  return rb->nlocal == 0;
}


int laws_shared_isempty(laws_t *rb) {
  return rb->tail == rb->split;
}


int laws_isempty(laws_t *rb) {
  return laws_local_isempty(rb) && laws_shared_isempty(rb);
}


int laws_local_size(laws_t *rb) {
  return rb->nlocal;
}


int laws_shared_size(laws_t *rb) {
  if (laws_shared_isempty(rb)) // Shared is empty
    return 0;
  else if (rb->tail < rb->split)   // No wrap-around
    return rb->split - rb->tail;
  else                             // Wrap-around
    return rb->split + rb->max_size - rb->tail;
}


int laws_public_size(laws_t *rb) {
  if (rb->vtail == rb->split) {    // Public is empty
    assert (rb->tail == rb->itail && rb->tail == rb->split);
    return 0;
  }
  else if (rb->vtail < rb->split)  // No wrap-around
    return rb->split - rb->vtail;
  else                             // Wrap-around
    return rb->split + rb->max_size - rb->vtail;
}


int laws_size(void *b) {
  laws_t *rb = (laws_t *)b;
  return laws_local_size(rb) + laws_shared_size(rb);
}


/*==================== SYNCHRONIZATION ====================*/


void laws_lock(laws_t *rb, int proc) {
  synch_mutex_lock(&rb->lock, proc);
}


int laws_trylock(laws_t *rb, int proc) {
  return synch_mutex_trylock(&rb->lock, proc);
}


void laws_unlock(laws_t *rb, int proc) {
  synch_mutex_unlock(&rb->lock, proc);
}


/*==================== SPLIT MOVEMENT ====================*/


int laws_reclaim_space(laws_t *rb) {
  GTC_ENTRY();
  int reclaimed = 0;
  int vtail = rb->vtail;
  int itail = rb->itail; // Capture these values since we are doing this
  int tail  = rb->tail;  // without a lock
  TC_START_TIMER(rb->tc, reclaim);
  if (vtail != tail && itail == tail) {
    rb->vtail = tail;
    if (tail > vtail)
      reclaimed = tail - vtail;
    else
      reclaimed = rb->max_size - vtail + tail;

    assert(reclaimed > 0);
  }

  rb->nreccalls++;
  TC_STOP_TIMER(rb->tc, reclaim);
  GTC_EXIT(reclaimed);
}



void laws_ensure_space(laws_t *rb, int n) {
  GTC_ENTRY();
  // Ensure that there is enough free space in the queue.  If there isn't
  // wait until others finish their deferred copies so we can reclaim space.
  TC_START_TIMER(rb->tc, ensure);
  if (rb->max_size - (laws_local_size(rb) + laws_public_size(rb)) < n) {
    laws_lock(rb, rb->procid);
    {
      if (rb->max_size - laws_size(rb) < n) {
        // Error: amount of reclaimable space is less than what we need.
        // Try increasing the size of the queue.
        printf("laws: Error, not enough space in the queue to push %d elements\n", n);
        laws_print(rb);
        assert(0);
      }
      rb->waiting = 1;
      while (laws_reclaim_space(rb) == 0) /* Busy Wait */ ;
      rb->waiting = 0;
      rb->nwaited++;
    }
    laws_unlock(rb, rb->procid);
  }
  TC_STOP_TIMER(rb->tc, ensure);
  GTC_EXIT();
}



void laws_release(laws_t *rb) {
  GTC_ENTRY();
  // Favor placing work in the shared portion -- if there is only one task
  // available this scheme will put it in the shared portion.
  TC_START_TIMER(rb->tc, release);
  if (laws_local_size(rb) > 0 && laws_shared_size(rb) == 0) {
    int amount  = laws_local_size(rb)/2 + laws_local_size(rb) % 2;
    rb->nlocal -= amount;
    rb->split   = (rb->split + amount) % rb->max_size;
    rb->nrelease++;
    
    // indicate to other intranode processes that there's work here
    shmem_atomic_or(rb->gaddr, 1, rb->root);

    gtc_lprintf(DBGSHRB, "release: local size: %d shared size: %d\n", laws_local_size(rb), laws_shared_size(rb));
  }
  TC_STOP_TIMER(rb->tc, release);
  GTC_EXIT();
}


void laws_release_all(laws_t *rb) {
  GTC_ENTRY();
  int amount  = laws_local_size(rb);
  rb->nlocal -= amount;
  rb->split   = (rb->split + amount) % rb->max_size;
  shmem_atomic_or(rb->gaddr, 1, rb->root);
  rb->nrelease++;
  GTC_EXIT();
}


int laws_reacquire(laws_t *rb) {
  GTC_ENTRY();
  int amount = 0;

  TC_START_TIMER(rb->tc, reacquire);
  // Favor placing work in the local portion -- if there is only one task
  // available this scheme will put it in the local portion.
  laws_lock(rb, rb->procid);
  {
    if (laws_shared_size(rb) > laws_local_size(rb)) {
      int diff    = laws_shared_size(rb) - laws_local_size(rb);
      amount      = diff/2 + diff % 2;
      rb->nlocal += amount;
      rb->split   = (rb->split - amount);
      if (rb->split < 0)
        rb->split += rb->max_size;
      rb->nreacquire++;
      if (laws_shared_isempty(rb)) {
          shmem_atomic_and(rb->gaddr, 0, rb->root);
      }
      gtc_lprintf(DBGSHRB, "reacquire: local size: %d shared size: %d\n", laws_local_size(rb), laws_shared_size(rb));
    }

    // Assertion: laws_local_isempty(rb) => laws_isempty(rb)
    assert(!laws_local_isempty(rb) || (laws_isempty(rb) && laws_local_isempty(rb)));
  }
  laws_unlock(rb, rb->procid);
  TC_STOP_TIMER(rb->tc, reacquire);

  GTC_EXIT(amount);
}


/*==================== PUSH OPERATIONS ====================*/


static inline void laws_push_n_head_impl(laws_t *rb, int proc, void *e, int n, int size) {
  int head, old_head;

  assert(size <= rb->elem_size);
  assert(size == rb->elem_size || n == 1);  // n > 1 ==> size == rb->elem_size
  assert(proc == rb->procid);
  TC_START_TIMER(rb->tc, pushhead);

  // Make sure there is enough space for n elements
  laws_ensure_space(rb, n);

  // Proceed with the push
  old_head    = laws_head(rb);
  rb->nlocal += n;
  head        = laws_head(rb);

  if (head > old_head || old_head == rb->max_size - 1) {
    memcpy(laws_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, n*size);
  }

  // This push wraps around, break it into two parts
  else {
    int part_size = rb->max_size - 1 - old_head;

    memcpy(laws_elem_addr(rb, proc, old_head+1), e, part_size*size);
    memcpy(laws_elem_addr(rb, proc, 0), laws_buff_elem_addr(rb, e, part_size), (n - part_size)*size);
  }
  TC_STOP_TIMER(rb->tc, pushhead);
}

void laws_push_head(laws_t *rb, int proc, void *e, int size) {
  GTC_ENTRY();
  // laws_push_n_head_impl(rb, proc, e, 1, size);
  int old_head;

  assert(size <= rb->elem_size);
  assert(proc == rb->procid);

  // Make sure there is enough space for n elements
  laws_ensure_space(rb, 1);

  // Proceed with the push
  old_head    = laws_head(rb);
  rb->nlocal += 1;

  memcpy(laws_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, size);

  // printf("(%d) pushed head\n", rb->procid);
  GTC_EXIT();
}

void laws_push_n_head(void *b, int proc, void *e, int n) {
  GTC_ENTRY();
  laws_t *rb = (laws_t *)b;
  laws_push_n_head_impl(rb, proc, e, n, rb->elem_size);
  GTC_EXIT();
}


void *laws_alloc_head(laws_t *rb) {
  GTC_ENTRY();
  // Make sure there is enough space for 1 element
  laws_ensure_space(rb, 1);

  rb->nlocal += 1;

  GTC_EXIT(laws_elem_addr(rb, rb->procid, laws_head(rb)));
}

/*==================== POP OPERATIONS ====================*/


int laws_pop_head(void *b, int proc, void *buf) {
  GTC_ENTRY();
  laws_t *rb = (laws_t *)b;
  int   old_head = 0;
  int   buf_valid = 0;

  assert(proc == rb->procid);

  // If we are out of local work, try to reacquire
  if (laws_local_isempty(rb))
    laws_reacquire(rb);

  if (laws_local_size(rb) > 0) {
    old_head = laws_head(rb);

    memcpy(buf, laws_elem_addr(rb, proc, old_head), rb->elem_size);

    rb->nlocal--;
    buf_valid = 1;
  }

  //printf("popped head %d\n", old_head);
  // Assertion: !buf_valid => laws_isempty(rb)
  assert(buf_valid || (!buf_valid && laws_isempty(rb)));

  // printf("(%d) popped head: head num: %d\n", rb->procid, old_head);

  GTC_EXIT(buf_valid);
}


int laws_pop_tail(laws_t *rb, int proc, void *buf) {
  GTC_ENTRY();
  GTC_EXIT(laws_pop_n_tail(rb, proc, 1, buf, STEAL_HALF));
}

/* Pop up to N elements off the tail of the queue, putting the result into the user-
 *  supplied buffer.
 *
 *  @param myrb  Pointer to the RB
 *  @param proc  Process to perform the pop on
 *  @param n     Requested/Max. number of elements to pop.
 *  @param e     Buffer to store result in.  Should be rb->elem_size*n bytes big and
 *               should also be allocated with laws_malloc().
 *  @param steal_vol Enumeration that selects between different schemes for determining
 *               the amount we steal.
 *  @param trylock Indicates whether to use trylock or lock.  Using trylock will result
 *               in a fail return value when trylock does not succeed.
 *
 *  @return      The number of tasks stolen or -1 on failure
 */

static inline int laws_pop_n_tail_impl(laws_t *myrb, int proc, int n, void *e, int steal_vol, int trylock) {
  laws_t trb;
  TC_START_TIMER(myrb->tc, poptail);
  __gtc_marker[1] = 3;
  // Attempt to get the lock
  if (trylock) {
    if (!laws_trylock(myrb, proc)) {
      return -1;
    }
  } else {
    laws_lock(myrb, proc);
  }

  // Copy the remote RB's metadata
  shmem_getmem(&trb, myrb, sizeof(laws_t), proc);

  switch (steal_vol) {
    case STEAL_HALF:
      n = MIN(laws_shared_size(&trb)/2 + laws_shared_size(&trb) % 2, n);
      break;
    case STEAL_ALL:
      n = MIN(laws_shared_size(&trb), n);
      break;
    case STEAL_CHUNK:
      // Get as much as possible up to N.
      n = MIN(n, laws_shared_size(&trb));
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

    loc_addr    = &new_tail;
    rem_addr    = &myrb->tail;
    xfer_size   = 1*sizeof(int);
    if (new_tail == (&trb)->split) {
        shmem_atomic_and((&trb)->gaddr, 0, (&trb)->root);
    }
    shmem_putmem(rem_addr, loc_addr, xfer_size, proc);

    laws_unlock(myrb, proc); // Deferred copy unlocks early

    // Transfer work into the local buffer
    if ((&trb)->tail + (n-1) < (&trb)->max_size) {    // No need to wrap around

      shmem_getmem_nbi(e, laws_elem_addr(myrb, proc, (&trb)->tail), n * (&trb)->elem_size, proc);    // Store n elems, starting at remote tail, in e
      
      shmem_quiet();

    } else {    // Need to wrap around
      int part_size  = (&trb)->max_size - (&trb)->tail;

      shmem_getmem_nbi(laws_buff_elem_addr(&trb, e, 0), laws_elem_addr(myrb, proc, (&trb)->tail), part_size * (&trb)->elem_size, proc);

      shmem_getmem_nbi(laws_buff_elem_addr(&trb, e, part_size), laws_elem_addr(myrb, proc, 0), (n - part_size) * (&trb)->elem_size, proc);

      shmem_quiet();

    }

    /*
    int *steal_from;
    for (int i = 1; i < n + 1; i++) {
        steal_from = (int *)((e + (i * (&trb)->elem_size)) + 8);
        printf("%d, ", *(steal_from - 4));
    }
    printf("\n");
    */

#ifndef SDC_NODC
    // Accumulate itail_inc onto the victim's intermediate tail
    {
      int itail_inc;
      //long int stride = 1;
      //int count = sizeof(int);

      // How much should we add to the itail?  If we caused a wraparound, we need to also wrap itail.
      if (new_tail > (&trb)->tail)
        itail_inc = n;
      else
        itail_inc = n - (&trb)->max_size;
      shmem_atomic_fetch_add(&(myrb->itail), itail_inc, proc);

      shmem_quiet();
    }
#else
    shmem_quiet();
    laws_unlock(myrb, proc);
#endif

  } else /* (n <= 0) */ {
    laws_unlock(myrb, proc);
  }
  TC_STOP_TIMER(myrb->tc, poptail);
  __gtc_marker[1] = 0;
  return n;
}

int laws_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
  GTC_ENTRY();
  laws_t *myrb = (laws_t *)b;
  GTC_EXIT(laws_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 0));
}

int laws_try_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
  GTC_ENTRY();
  laws_t *myrb = (laws_t *)b;
  GTC_EXIT(laws_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 1));
}
