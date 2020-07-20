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
  rb->vtail      = 0;
  rb->steal_val  = 0;
  rb->cur        = 0;
  rb->last       = 1;
  rb->split      = 0;
  rb->waiting    = 0;
  rb->nshared    = 0;
  rb->nrelease   = 0;
  rb->nreacquire = 0;
  rb->nwaited    = 0;
  rb->nreclaimed = 0;

  memset(rb->claimed, 0, sizeof(rb->claimed));
  memset(rb->completed, 0, sizeof(rb->completed));
  rb->completed[rb->last].done = 1;

  for(int i = 0; i < rb->nproc; i++)
    rb->targets[i] = FullQueue;

}


void saws_shrb_destroy(saws_shrb_t *rb) {
  free(rb->targets);
  shmem_free(rb);
}


/*================== HELPER FUNCTIONS ===================*/

void saws_shrb_print(saws_shrb_t *rb) {
  printf("rb: %p {\n", rb);
  printf("   procid  = %d\n", rb->procid);
  printf("   nproc  = %d\n", rb->nproc);
  printf("   nlocal    = %d\n", rb->nlocal);
  printf("   head      = %d\n", saws_shrb_head(rb));
  printf("   split     = %"PRId64"\n", rb->split);
  printf("   tail      = %"PRId64"\n", rb->tail);
  printf("   max_size  = %d\n", rb->max_size);
  printf("   elem_size = %d\n", rb->elem_size);
  printf("   local_size = %d\n", saws_shrb_local_size(rb));
  printf("   shared_size= %d\n", saws_shrb_shared_size(rb));
  printf("   public_size= %d\n", saws_shrb_public_size(rb));
  printf("   size       = %d\n", saws_shrb_size(rb));
  printf("   a_steals   = %ld\n", (rb->steal_val >> 40));
  printf("   i_tasks    = %ld\n", ((rb->steal_val >> 19) & 0x1F));
  printf("   vtail      = %ld\n", rb->steal_val & 0x00000000007FFFF);
  printf("   current epoch = %d\n", rb->cur);
  printf("}\n");
}

void print_epoch(saws_shrb_t *rb) {
  printf("\nprocid    = %d\n", rb->procid);
  int c = rb->cur;
  int x = 0;
ag:
  printf("epoch   = %d\n",c);
  printf("  itasks    = %ld\n", rb->completed[c].itasks);
  printf("  vtail     = %ld\n", rb->completed[c].vtail);
  printf("  done?     = %d\n", rb->completed[c].done);
  printf("  maxsteals = %d\n", rb->completed[c].maxsteals);
  printf("  status: ");
  for (int i = 0; i < rb->completed[c].maxsteals; i++)
    printf(" [%d] ", rb->completed[c].status[i]);
  printf("\nprev: \n");
  if (x == 0) { c = rb->last; x = 1; goto ag;}
}

static inline uint64_t saws_set_stealval(int64_t valid, uint64_t itasks, int64_t tail) {
  uint64_t steal_val = 0;
  steal_val   |= valid << 38;
  steal_val   |= itasks << 19;
  steal_val   |= tail;
  return steal_val;
}

static inline uint64_t saws_get_stealval(uint64_t steal_val, uint64_t *asteals, uint64_t *itasks, int64_t *tail) {
  uint64_t valid;
  *asteals   =    (steal_val >> 40)  & 0x0000000000FFFFFF;
  valid      =    (steal_val >> 38)  & 0x0000000000000003;
  *itasks    =    (steal_val >> 19)  & 0x000000000007FFFF;
  *tail      =    steal_val          & 0x000000000007FFFF;
  return valid;
}


static inline uint64_t saws_disable_steals(saws_shrb_t *rb) {
  static uint64_t val = SAWS_MAX_EPOCHS << 38;
  return shmem_atomic_fetch_or(&rb->steal_val, val, rb->procid);
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
  return saws_shrb_shared_size(rb);
  /*
     if (rb->vtail == rb->split) {    // Public is empty
     return 0;
     }
     else if (rb->vtail < rb->split)  // No wrap-around
     return rb->split - rb->vtail;
     else                             // Wrap-around
     return rb->split + rb->max_size - rb->vtail;
     */
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
  int64_t vtail, valid;
  int reclaimed = 0;
  uint32_t sum = 0;

  // copy stealval
  // something's wrong here...
  uint64_t sv = shmem_atomic_fetch(&rb->steal_val, rb->procid);
  valid = saws_get_stealval(sv, &asteals, &itasks, &vtail);
  assert(valid < SAWS_MAX_EPOCHS);
  // check if any steals from the last epoch have been completed
  // if so, update tail index accordingly.
  if (!rb->completed[rb->last].done) {
    for (int i = 0; i < rb->completed[rb->last].maxsteals; i++)
      sum += rb->completed[rb->last].status[i];

    if (sum == rb->completed[rb->last].itasks) {
      rb->tail = rb->completed[rb->cur].vtail;
      memset(&rb->completed[rb->last], 0,  sizeof(rb->completed[rb->last]));
      rb->completed[rb->last].done = 1;
    }
  }

  sum = 0;
  for (int i = 0; i < rb->completed[rb->cur].maxsteals; i++) {
    if (rb->completed[rb->cur].status[i] == 0)
      break;
    sum += rb->completed[rb->cur].status[i];
  }
  if (sum == rb->completed[rb->cur].itasks) {
    rb->completed[rb->cur].done = 1;

  }
  if (sum > 0) {
    rb->tail = rb->completed[rb->cur].vtail + sum % rb->max_size;
  }
  assert(saws_shrb_shared_isempty(rb) || rb->completed[rb->cur].done != 1 || rb->completed[rb->last].done != 1);
  rb->nreccalls++;
  assert(rb->tail <= rb->max_size);
  TC_STOP_TIMER(rb->tc, reclaim);
  return reclaimed;
}



void saws_shrb_ensure_space(saws_shrb_t *rb, int n) {
  // Ensure that there is enough free space in the queue.  If there isn't
  // wait until others finish their deferred copies so we can reclaim space.
  TC_START_TIMER(rb->tc, ensure);
  if (rb->max_size - (saws_shrb_local_size(rb) + saws_shrb_public_size(rb)) < n) {
    saws_shrb_reclaim_space(rb);
    if (rb->max_size - saws_shrb_size(rb) < n) {
      // Error: amount of reclaimable space is less than what we need.
      // Try increasing the size of the queue.
      printf("SAWS_SHRB: Error, not enough space in the queue to push %d elements\n", n);
      saws_shrb_print(rb);
      assert(0);
    }
  }
  TC_STOP_TIMER(rb->tc, ensure);
}



void saws_shrb_release(saws_shrb_t *rb) {

  TC_START_TIMER(rb->tc, release);

  if (saws_shrb_local_size(rb) > 0 && (saws_shrb_shared_size(rb) == 0)) {
    uint64_t  steal_val;
    uint64_t nshared;
    nshared  = rb->nlocal / 2 + rb->nlocal % 2;
    rb->nlocal  -= nshared;
    rb->split    = (rb->split + nshared) % rb->max_size;

    gtc_lprintf(DBGSHRB, "releasing %d task\tsplit: %d  tail: %d\n", nshared, rb->split, rb->tail);

    // initialize epoch
    rb->completed[rb->cur].itasks    = nshared;
    rb->completed[rb->cur].maxsteals = saws_max_steals(nshared);
    rb->completed[rb->cur].done      = 0;
    rb->completed[rb->cur].vtail     = rb->tail;
    memset(&rb->completed[rb->cur].status, 0, sizeof(rb->completed[rb->cur].status));

    steal_val = saws_set_stealval(rb->cur, nshared, rb->tail);
    shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);
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

  steal_val = saws_set_stealval(rb->cur, amount, rb->tail);
  shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);
  rb->nrelease++;
}


void saws_shrb_reacquire(saws_shrb_t *rb) {
  uint64_t steal_val, asteals, itasks, amount;
  uint64_t sum = 0;
  int64_t vtail;
  int tasks_left, stolen;

<<<<<<< HEAD
  if ((saws_shrb_shared_size(rb) <= rb->nlocal) || rb->nlocal != 0)
    return;

  TC_START_TIMER(rb->tc, reacquire);

  // disable steals and determine shared queue state
  steal_val = saws_disable_steals(rb);
  saws_get_stealval(steal_val, &asteals, &itasks, &vtail);
  gtc_dprintf("steals disabled : tail %d split: %d itasks: %d asteals: %d\n", rb->tail, rb->split, itasks, asteals);

  // assert that all steals from the last epoch have completed.
  if (!rb->completed[rb->last].done) {

    for (int i = 0; i < rb->completed[rb->last].maxsteals; i++) {
      sum += rb->completed[rb->last].status[i];
    }

    if (sum != rb->completed[rb->last].itasks) {
      gtc_dprintf("incomplete tasks over multiple epochs -- dying now\n");
      exit(1);
    }
  }

  memset(&rb->completed[rb->last], 0, sizeof(rb->completed[rb->last]));

  //print_epoch(rb);
  // switch epochs
  rb->cur  = (rb->cur + 1)  % SAWS_MAX_EPOCHS;
  rb->last = (rb->last + 1) % SAWS_MAX_EPOCHS;
  //print_epoch(rb);

  // determine the number of unclaimed tasks available in queue
  int temp = asteals;
  for (stolen = 0, tasks_left = itasks; temp > 0; temp--) {
    tasks_left  = (tasks_left != 1) ? tasks_left >> 1 : 1; // # of stolen tasks for this steal
    stolen += tasks_left;                                  // add to total amount stolen
    tasks_left  = itasks - stolen;
  }
  if (tasks_left == 1) amount = 1;
  else amount = tasks_left / 2 + tasks_left % 2;

  rb->nlocal += amount;
  rb->split   = (rb->split - amount);
  if (rb->split < 0)
    rb->split += rb->max_size;

  gtc_lprintf(DBGSHRB, "Reacquiring %d tasks of %d\n", amount, tasks_left);

  // update tail wrt completed steals
  temp = rb->completed[rb->last].vtail;
  for (int i = 0; i < rb->completed[rb->last].maxsteals; i++) {
    gtc_dprintf("c[%d] = %d ", i, rb->completed[rb->last].status[i]);
    if (rb->completed[rb->last].status[i] == 0)
      break;
    temp += rb->completed[rb->last].status[i];
  }
  rb->tail = temp % rb->max_size; // double check this...
  gtc_dprintf(" -- temp: %d tail %d\n", temp, rb->tail);

  // correct old epoch incase there's outstanding steals
  rb->completed[rb->last].itasks = stolen;
  rb->completed[rb->last].maxsteals = asteals;

  // set current epoch
  rb->completed[rb->cur].itasks = tasks_left - amount;
  rb->completed[rb->cur].maxsteals = saws_max_steals(tasks_left - amount);
  gtc_dprintf("tail: %d split: %d itasks: %d computed: %d\n", rb->tail,
      rb->split, rb->completed[rb->cur].itasks, rb->split - rb->completed[rb->cur].itasks);
  //assert( (uint32_t )rb->tail == (rb->split - rb->completed[rb->cur].itasks));
  rb->completed[rb->cur].done = 0;
  rb->completed[rb->cur].vtail = rb->tail;

  //if (rb->procid == 0)
  //    print_epoch(rb);
  steal_val = saws_set_stealval(rb->cur, tasks_left - amount, rb->completed[rb->cur].vtail);
  shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);

  assert(!saws_shrb_local_isempty(rb) || (saws_shrb_isempty(rb) && saws_shrb_local_isempty(rb)));
  TC_STOP_TIMER(rb->tc, reacquire);
}


/*==================== PUSH OPERATIONS ====================*/


static inline void saws_shrb_push_n_head_impl(saws_shrb_t *rb, int proc, void *e, int n, int size) {
  int head, old_head;

  assert(size <= rb->elem_size);
  assert(size == rb->elem_size || n == 1);  // n > 1 ==> size == rb->elem_size
  assert(proc == rb->procid);
<<<<<<< HEAD
  //TC_START_TIMER(rb->tc, pushhead);
=======
  TC_START_TIMER(rb->tc, pushhead);
>>>>>>> c148b3c1fd737c4481ab366efd9e7d447cb84b54

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
<<<<<<< HEAD
  //TC_START_TIMER(rb->tc, pushhead);
=======
  TC_STOP_TIMER(rb->tc, pushhead);
>>>>>>> c148b3c1fd737c4481ab366efd9e7d447cb84b54
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
  uint64_t steal_val, asteals, tasks_left, itasks, increment, maxsteals;
  int64_t  rtail;
  void *rptr = NULL;
  increment = 1L << 40;

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

  shmem_quiet();
  if (valid >= SAWS_MAX_EPOCHS) {
    printf("\n%din steals  valid %d\n", proc, valid);
    return 0;
  }
  maxsteals = saws_max_steals(itasks);

  if (asteals >= maxsteals) {
    myrb->targets[proc] = EmptyQueue;
    return 0;
  } else if (myrb->targets[proc] == EmptyQueue) {
    myrb->targets[proc] = FullQueue;
    goto test;
  }

  gtc_lprintf(DBGSHRB, "Calculating steal volume, maxsteals %"PRIu64", asteals %"PRIu64" itasks %"PRIu64"\n",
      maxsteals, asteals, itasks);

  // calculate number of tasks already stolen
  int index = asteals;
  for (stolen = 0, tasks_left = itasks; asteals > 0; asteals--) {
    tasks_left  = (tasks_left != 1) ? tasks_left >> 1 : 1; // # of stolen tasks for this steal
    stolen += tasks_left;                          // add to total amount stolen
    tasks_left  = itasks - stolen;
  }
  // calculate steal volume for our attempt
  ntasks = (tasks_left != 1) ? tasks_left >> 1 : 1;
  assert(ntasks > 0);
  if(ntasks <= 0)
    return 0;
  gtc_lprintf(DBGSHRB, "stealing %d tasks from (%d), starting at index %d\n", ntasks, proc, rtail + stolen);

  // determine base address of tasks to steal
  rptr = &myrb->q[0] + ((rtail + stolen) * myrb->elem_size);

  // check to see if the task block wraps around the end of the queue
  if (rtail + stolen + ntasks < myrb->max_size) {
    // No wrap
    shmem_getmem_nbi(e, rptr, ntasks * myrb->elem_size, proc);

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

  }
  gtc_lprintf(DBGSHRB, "sending completion to epoch %d index %d\n", valid, index);
  shmem_atomic_add(&myrb->completed[valid].status[index], ntasks, proc);


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
