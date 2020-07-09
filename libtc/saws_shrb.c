/*************************************************************/
/*                                                           */
/*  saws_shrb.c - scioto atomic ring buffer q implementation */
/*    (c) 2020 see COPYRIGHT in top-level                    */
/*                                                           */
/*************************************************************/

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <mutex.h>

#include "tc.h"
#include "saws_shrb.h"

#define LOCKQUEUE 0x000000000000000
#define FullQueue 1
#define EmptyQueue 0

/**
 * SHMEM Atomic Work Stealing (SAWS) Task Queue
 * ================================================
 *
 * This queue is split into two parts, as shown in the diagram below: a part that is for
 * local access only (allowing for lock-free local interaction with the queue), a part for
 * shared access. The remaining portion of the queue is free.
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
 *
 */

saws_shrb_t *saws_shrb_create(int elem_size, int max_size, tc_t *tc) {
  saws_shrb_t  *rb;
  int procid, nproc;
  uint32_t *targets;
  setbuf(stdout, NULL);

  procid = shmem_my_pe();
  nproc = shmem_n_pes();

  gtc_lprintf(DBGSHRB, "  Thread %d: saws_shrb_create()\n", procid);

  // Allocate the struct and the buffer contiguously in shared space
  rb = shmem_malloc(sizeof(saws_shrb_t) + elem_size*max_size);

  targets = (uint32_t *) calloc(nproc, sizeof(uint32_t));

  rb->procid    = procid;
  rb->nproc     = nproc;
  rb->elem_size = elem_size;
  rb->max_size  = max_size;
  rb->targets   = targets;
  rb->tc        = tc;

  saws_shrb_reset(rb);

  synch_mutex_init(&rb->lock);

  shmem_barrier_all();

  return rb;
}


void saws_shrb_reset(saws_shrb_t *rb) {

  rb->nlocal     = 0;
  rb->tail       = 0;
  rb->itail      = 0;
  rb->vtail      = 0;
  rb->steal_val  = 0;
  rb->split      = 0;
  rb->waiting    = 0;
  rb->nshared    = 0;
  rb->nrelease   = 0;
  rb->nreacquire = 0;
  rb->nwaited    = 0;
  rb->nreclaimed = 0;

  memset(rb->claimed, 0, sizeof(rb->claimed));

  for(int i = 0; i < rb->nproc; i++)
    rb->targets[i] = FullQueue;

}


void saws_shrb_destroy(saws_shrb_t *rb) {
  free(rb->targets);
  shmem_free(rb);
}


void saws_shrb_print(saws_shrb_t *rb) {
  printf("rb: %p {\n", rb);
  printf("   procid  = %d\n", rb->procid);
  printf("   nproc  = %d\n", rb->nproc);
  printf("   nlocal    = %d\n", rb->nlocal);
  printf("   head      = %d\n", saws_shrb_head(rb));
  printf("   split     = %"PRId64"\n", rb->split);
  printf("   tail      = %"PRId64"\n", rb->tail);
  printf("   itail     = %"PRId64"\n", rb->itail);
  printf("   vtail     = %"PRId64"\n", rb->vtail);
  printf("   max_size  = %d\n", rb->max_size);
  printf("   elem_size = %d\n", rb->elem_size);
  printf("   local_size = %d\n", saws_shrb_local_size(rb));
  printf("   shared_size= %d\n", saws_shrb_shared_size(rb));
  printf("   public_size= %d\n", saws_shrb_public_size(rb));
  printf("   size       = %d\n", saws_shrb_size(rb));
  printf("   a_steals   = %ld\n", (rb->steal_val >> 39));
  printf("   i_tasks    = %ld\n", ((rb->steal_val >> 19) & 0x1F));
  printf("}\n");
}

/*================== HELPER FUNCTIONS ===================*/

static inline uint64_t saws_set_stealval(int64_t valid, uint64_t itasks, int64_t tail) {
  uint64_t steal_val = 0;
  steal_val   |= valid << 38;
  steal_val   |= itasks << 19;
  steal_val   |= tail;
  return steal_val;
}

static inline uint64_t saws_get_stealval(uint64_t steal_val, uint64_t *asteals, uint64_t *itasks, int64_t *tail) {
  uint64_t valid;
  *asteals   =    (steal_val >> 39)  & 0x0000000001FFFFFF;
  valid      =    (steal_val >> 38)  & 0x0000000000000001;
  *itasks    =    (steal_val >> 19)  & 0x000000000007FFFF;
  *tail      =    steal_val          & 0x000000000007FFFF;
  return valid;
}


static inline uint64_t saws_disable_steals(saws_shrb_t *rb) {
  static uint64_t val = 1;
  val = ~(val << 38); // all ones except for valid
  return shmem_atomic_fetch_and(&rb->steal_val, val, rb->procid);
}

static inline void saws_enable_steals(saws_shrb_t *rb) {
  static uint64_t val = 1;
  val <<= 38; // all zeros except for valid
  shmem_atomic_or(&rb->steal_val, val, rb->procid);
}


static inline int saws_max_steals(uint64_t itasks) {
  uint32_t curr,cnt, total = 0;

  for (cnt = 0, curr = itasks; total != itasks; cnt++) {
    curr = (curr != 1) ? curr >> 1 : 1;
    total += curr;
    curr = itasks - total;
  }
  return cnt;
}


static inline void saws_update_claimed(saws_shrb_t *rb, uint64_t itasks) {
  uint64_t remaining;

  memset(rb->claimed, 0, sizeof(rb->claimed));

  remaining = itasks;
  for (int i=0; remaining > 0; i++) {
    rb->claimed[i] = remaining / 2;
    remaining -= rb->claimed[i];
  }
}


static inline int log2_ceil(uint64_t x) {
  int rv = -1;

  while (x) { rv++ ; x = x >> 1; }
  return rv + 1;
}


unsigned int powOf2(unsigned int n) {
  unsigned int count = 0;

  while (n) {
    count += n & 1;
    n >>= 1;
  }
  return count == 1;
}

//prints binary representation of an integer
void itobin(int v) {
  unsigned int mask=1<<((sizeof(int)<<3)-1);

  while(mask) {
    printf("%d", (v&mask ? 1 : 0));
    mask >>= 1;
  }
}


/*==================== STATE QUERIES ====================*/


int saws_shrb_head(saws_shrb_t *rb) {
  return (rb->split + rb->nlocal - 1) % rb->max_size;
}


int saws_shrb_local_isempty(saws_shrb_t *rb) {
  return rb->nlocal == 0;
}


int saws_shrb_shared_isempty(saws_shrb_t *rb) {
  return rb->tail == rb->split;
}


int saws_shrb_isempty(saws_shrb_t *rb) {
  return saws_shrb_local_isempty(rb) && saws_shrb_shared_isempty(rb);
}


int saws_shrb_local_size(saws_shrb_t *rb) {
  return rb->nlocal;
}


int saws_shrb_shared_size(saws_shrb_t *rb) {
  if (saws_shrb_shared_isempty(rb)) // Shared is empty
    return 0;
  else if (rb->tail < rb->split)   // No wrap-around
    return rb->split - rb->tail;
  else                             // Wrap-around
    return rb->split + rb->max_size - rb->tail;
}


int saws_shrb_public_size(saws_shrb_t *rb) {
  if (rb->vtail == rb->split) {    // Public is empty
    return 0;
  }
  else if (rb->vtail < rb->split)  // No wrap-around
    return rb->split - rb->vtail;
  else                             // Wrap-around
    return rb->split + rb->max_size - rb->vtail;
}


int saws_shrb_size(void *b) {
  saws_shrb_t *rb = (saws_shrb_t *)b;
  return saws_shrb_local_size(rb) + saws_shrb_shared_size(rb);
}


/*==================== SYNCHRONIZATION ====================*/


void saws_shrb_lock(saws_shrb_t *rb, int proc) {
  synch_mutex_lock(&rb->lock, proc);
}


int saws_shrb_trylock(saws_shrb_t *rb, int proc) {
  return synch_mutex_trylock(&rb->lock, proc);
}


void saws_shrb_unlock(saws_shrb_t *rb, int proc) {
  synch_mutex_unlock(&rb->lock, proc);
}


/*==================== SPLIT MOVEMENT ====================*/

int saws_shrb_reclaim_space(saws_shrb_t *rb) {
  TC_START_TIMER(rb->tc, reclaim);
  uint64_t asteals, itasks;
  int64_t vtail, ctail;
  int tail  = rb->tail;     // captured without lock
  int reclaimed = 0;

  // XXX - worry about this case:
  //   steal starts | acquire | reclaim | steal completes
  //   asteals is updated in acquire

  // copy stealval (also without lock)
  saws_get_stealval(rb->steal_val, &asteals, &itasks, &vtail);

  // find tail index for claimed tasks
  ctail = vtail + rb->claimed[asteals];
  if (ctail >= rb->max_size)
    ctail -= rb->max_size;

  // if ctail == tail, then there are no in-flight steals
  if ((vtail != tail) && (ctail == tail)) {
    rb->vtail = tail; // recover completed stolen tasks (steal_val vtail not updated)
    if (tail > vtail)
      reclaimed = tail - vtail;
    else
      reclaimed = rb->max_size - vtail + tail;
    rb->nreclaimed++;
  }
  rb->nreccalls++;

  TC_STOP_TIMER(rb->tc, reclaim);
  return reclaimed;
}



void saws_shrb_ensure_space(saws_shrb_t *rb, int n) {
  // Ensure that there is enough free space in the queue.  If there isn't
  // wait until others finish their deferred copies so we can reclaim space.
  TC_START_TIMER(rb->tc, ensure);
  if (rb->max_size - (saws_shrb_local_size(rb) + saws_shrb_public_size(rb)) < n) {
    //    saws_shrb_lock(rb, rb->procid);
    {
      saws_shrb_reclaim_space(rb);
      if (rb->max_size - saws_shrb_size(rb) < n) {
        // Error: amount of reclaimable space is less than what we need.
        // Try increasing the size of the queue.
        printf("SAWS_SHRB: Error, not enough space in the queue to push %d elements\n", n);
        saws_shrb_print(rb);
        assert(0);
      }
    }
    //  saws_shrb_unlock(rb, rb->procid);
  }
  TC_STOP_TIMER(rb->tc, ensure);
}



void saws_shrb_release(saws_shrb_t *rb) {
  TC_START_TIMER(rb->tc, release);
  uint64_t  steal_val, valid = 1, nshared;

  if (saws_shrb_local_size(rb) > 0 && (saws_shrb_shared_size(rb) == 0)) {
    nshared  = saws_shrb_local_size(rb) / 2 + saws_shrb_local_size(rb) % 2;

    rb->nlocal  -= nshared;
    rb->split    = (rb->split + nshared) % rb->max_size;

    gtc_lprintf(DBGSHRB, "releasing %d tasks\n", nshared);

    steal_val = saws_set_stealval(valid, nshared, rb->tail);
    shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);

    saws_update_claimed(rb, nshared);

    rb->nrelease++;

  }
  assert (rb->tail >= 0 && rb->tail < rb->max_size);
  TC_STOP_TIMER(rb->tc, release);
}


void saws_shrb_release_all(saws_shrb_t *rb) {
  uint64_t steal_val;
  int amount  = saws_shrb_local_size(rb);
  rb->nlocal -= amount;
  rb->split   = (rb->split + amount) % rb->max_size;

  steal_val = saws_set_stealval(1, amount, rb->tail);
  shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);
  saws_update_claimed(rb, amount);
  rb->nrelease++;
}


void saws_shrb_reacquire(saws_shrb_t *rb) {
  TC_START_TIMER(rb->tc, reacquire);
  uint64_t steal_val, asteals, itasks, tasks_left, amount;
  int64_t vtail;

  // disable steals and determine shared queue state
  steal_val = saws_disable_steals(rb);
  saws_get_stealval(steal_val, &asteals, &itasks, &vtail);

  // determine the number of unclaimed tasks available in queue
  tasks_left = itasks - rb->claimed[asteals];
  if ((tasks_left > 0) && (rb->nlocal == 0)) {
    amount = tasks_left/2 + tasks_left %2;
    rb->nlocal += amount;
    rb->split   = (rb->split - amount);
    if (rb->split < 0)
      rb->split += rb->max_size;

    // reset steal_val
    // XXX which tail to use here? update vtail? use vtail + rb->claimed[asteals]
    steal_val = saws_set_stealval(1, tasks_left - amount, rb->tail);
    shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);
    saws_update_claimed(rb, tasks_left - amount);
    rb->nreacquire++;
  } else {
    saws_enable_steals(rb);
  }

  //assert(!saws_shrb_local_isempty(rb) || (saws_shrb_isempty(rb) && saws_shrb_local_isempty(rb)));

  TC_STOP_TIMER(rb->tc, reacquire);
}


/*==================== PUSH OPERATIONS ====================*/


static inline void saws_shrb_push_n_head_impl(saws_shrb_t *rb, int proc, void *e, int n, int size) {
  int head, old_head;

  assert(size <= rb->elem_size);
  assert(size == rb->elem_size || n == 1);  // n > 1 ==> size == rb->elem_size
  assert(proc == rb->procid);
  TC_START_TIMER(rb->tc, pushhead);

    // Make sure there is enough space for n elements
    saws_shrb_ensure_space(rb, n);

  // Proceed with the push
  old_head    = saws_shrb_head(rb);
  rb->nlocal += n;
  head        = saws_shrb_head(rb);

  if (head > old_head || old_head == rb->max_size - 1) {
    memcpy(saws_shrb_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, n*size);
  }

  // This push wraps around, break it into two parts
  else {
    int part_size = rb->max_size - 1 - old_head;

    memcpy(saws_shrb_elem_addr(rb, proc, old_head+1), e, part_size*size);
    memcpy(saws_shrb_elem_addr(rb, proc, 0), saws_shrb_buff_elem_addr(rb, e, part_size), (n - part_size)*size);
  }
  TC_START_TIMER(rb->tc, pushhead);
}



void saws_shrb_push_head(saws_shrb_t *rb, int proc, void *e, int size) {

    int old_head;

    assert(size <= rb->elem_size);
    assert(proc == rb->procid);

    saws_shrb_ensure_space(rb, 1);

    old_head    = saws_shrb_head(rb);
    rb->nlocal += 1;

    memcpy(saws_shrb_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, size);

}



void saws_shrb_push_n_head(void *b, int proc, void *e, int n) {
    saws_shrb_t *rb = (saws_shrb_t *)b;
    saws_shrb_push_n_head_impl(rb, proc, e, n, rb->elem_size);
}



void *saws_shrb_alloc_head(saws_shrb_t *rb) {
    // Make sure there is enough space for 1 element
    saws_shrb_ensure_space(rb, 1);

    rb->nlocal += 1;

    return saws_shrb_elem_addr(rb, rb->procid, saws_shrb_head(rb));
}


/*==================== POP OPERATIONS ====================*/


int saws_shrb_pop_head(void *b, int proc, void *buf) {

    saws_shrb_t *rb = (saws_shrb_t *)b;
    int   old_head;
    int   buf_valid = 0;

    assert(proc == rb->procid);

    // If we are out of local work, try to reacquire
    if (saws_shrb_local_isempty(rb))
        saws_shrb_reacquire(rb);

    if (saws_shrb_local_size(rb) > 0) {
        old_head = saws_shrb_head(rb);

        memcpy(buf, saws_shrb_elem_addr(rb, proc, old_head), rb->elem_size);
        rb->nlocal--;
        buf_valid = 1;
    }

    return buf_valid;
}


int saws_shrb_pop_tail(saws_shrb_t *rb, int proc, void *buf) {
    return saws_shrb_pop_n_tail(rb, proc, 1, buf, STEAL_CHUNK);
}

/* Pop up to N elements off the tail of the queue, putting the result into the user-
 *  supplied buffer.
 *
 *  @param myrb  Pointer to the RB
 *  @param proc  Process to perform the pop on
 *  @param n     Requested/Max. number of elements to pop.
 *  @param e     Buffer to store result in.  Should be rb->elem_size*n bytes big and
 *               should also be allocated with saws_shrb_malloc().
 *  @param steal_vol Enumeration that selects between different schemes for determining
 *               the amount we steal.
 *  @param trylock Indicates whether to use trylock or lock.  Using trylock will result
 *               in a fail return value when trylock does not succeed.
 *
 *  @return      The number of tasks stolen or -1 on failure
 */
static inline int saws_shrb_pop_n_tail_impl(saws_shrb_t *myrb, int proc, int n, void *e, int steal_vol, int trylock) {
  TC_START_TIMER(myrb->tc, poptail);
  int valid, ntasks = 0, stolen = 0;
  uint64_t steal_val, asteals, stasks, itasks, increment, maxsteals;
  int64_t  rtail, tailinc;
  void *rptr = NULL;
  increment = 1L << 39;

  // if target is in empty mode
  //   grab remote stealval and check - if work, then retry else return 0
  // else
  //   claim work
  //   calculate steal volume
  //   wrap or no wrap?
  //     get tasks
  //     wait
  //   atomic advance tail (tail wrap or no wrap?)

test:

  if (myrb->targets[proc] == FullQueue)
    steal_val = shmem_atomic_fetch_add(&myrb->steal_val, increment, proc);
  else
    steal_val = shmem_atomic_fetch(&myrb->steal_val, proc);

  valid = saws_get_stealval(steal_val, &asteals, &itasks, &rtail);

  if (!valid) return 0;

  maxsteals = saws_max_steals(itasks);

  if (asteals > maxsteals) {
    myrb->targets[proc] = EmptyQueue;
    return 0;
  } else if (myrb->targets[proc] == EmptyQueue) {
    myrb->targets[proc] = FullQueue;
    goto test;
  }

  gtc_lprintf(DBGSHRB, "Calculating steal volume, maxsteals %"PRIu64", asteals %"PRIu64" itasks %"PRIu64"\n",
      maxsteals, asteals, itasks);

  // calculate number of tasks already stolen
  for (stolen = 0, stasks = itasks; asteals > 0; asteals--) {
    stasks  = (stasks != 1) ? stasks >> 1 : 1; // # of stolen tasks for this steal
    stolen += stasks;                          // add to total amount stolen
    stasks  = itasks - stolen;
  }
  // calculate steal volume for our attempt
  ntasks = (stasks != 1) ? stasks >> 1 : 1;

  gtc_lprintf(DBGSHRB, "stealing %d tasks from (%d), starting at index %d\n", ntasks, proc, rtail + stolen);

  // determine base address of tasks to steal
  rptr = &myrb->q[0] + ((rtail + stolen) * myrb->elem_size);

  // check to see if the task block wraps around the end of the queue
  if (rtail + stolen + ntasks < myrb->max_size) {
    // No wrap
    shmem_getmem_nbi(e, rptr, ntasks * myrb->elem_size, proc);
    tailinc = ntasks;

  } else { 
    // steal wraps, use two communications
    
    // calculate how many tasks from thosealready stolen to the end of the queue
    int part_size = myrb->max_size - stolen; 

    // steal from just after already stolen tasks
    shmem_getmem_nbi(saws_shrb_buff_elem_addr(myrb, e, 0), rptr, part_size * myrb->elem_size, proc);
    // steal from beginning of queue the remaining tasks
    shmem_getmem_nbi(saws_shrb_buff_elem_addr(myrb, e, part_size),
        saws_shrb_elem_addr(myrb, proc, 0),
        (ntasks - part_size) * myrb->elem_size, proc);

    // calculate wrapped tail increment
    tailinc = (ntasks - part_size) - (myrb->max_size - part_size);
    //printf("part size: %d tail inc: %"PRId64"\n", part_size);
  }

  shmem_atomic_add(&myrb->tail, tailinc, proc);


  shmem_quiet(); // this is required to wait for the non-blocking shmem_getmem_nbi's
  TC_STOP_TIMER(myrb->tc, poptail);
  return ntasks;
}


int saws_shrb_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
    saws_shrb_t *myrb = (saws_shrb_t *)b;
    return saws_shrb_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 0);
}


int saws_shrb_try_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
    saws_shrb_t *myrb = (saws_shrb_t *)b;
    return saws_shrb_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 1);
}
