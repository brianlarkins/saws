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


// TODO: How to handle uneven numbers of ranks (say we're not using all of the cores of a particular node)?
laws_local_t *laws_create(int elem_size, int max_size, tc_t *tc) {
  GTC_ENTRY();
  laws_local_t  *rb;
  int procid, nproc;

  setbuf(stdout, NULL);

  procid = shmem_my_pe();
  nproc = shmem_n_pes();

  gtc_lprintf(DBGSHRB, "  Thread %d: laws_create()\n", procid);

  // Allocate the struct and the buffer contiguously in shared space
  rb = gtc_shmem_malloc(sizeof(laws_local_t) + elem_size*max_size);

  // allocate memory for global array of shared metadata
  int cores_per_node = sysconf(_SC_NPROCESSORS_ONLN);
  rb->gaddrs = gtc_shmem_calloc(cores_per_node, sizeof(laws_global_t));
  //printf("Process %d: cores per node is %d\n", procid, cores_per_node);

  // local copy of the global array located in the root process
  // not using a separate array results in undefined behavior
  rb->global = calloc(cores_per_node, sizeof(laws_global_t));

  rb->rank_in_node = procid % cores_per_node;
  int rank_in_node = rb->rank_in_node;

  rb->procid  = procid;
  rb->nproc  = nproc;
  rb->elem_size = elem_size;
  rb->max_size  = max_size;
  rb->root = procid - rank_in_node;
  rb->our_root = rb->root;
  rb->alt_root = 0;
  rb->ncores = cores_per_node;
  rb->g_meta = &rb->global[rank_in_node];
  rb->gaddr = &rb->gaddrs[rank_in_node];
  // rb->release = 1;

  // Set local pointer in our global metadata to memory address of our local metadata
  // Needed to access the queue

  rb->tc = tc;

  // Initialize the lock (global and local)
  synch_mutex_init(&rb->lock);
  //synch_mutex_init(&rb->g_meta->lock);

  //synch_mutex_init(&rb->g_meta->lock);
  /*
  for (int i = 0; i < cores_per_node; i++) {
      synch_mutex_init(&rb->global[i].lock);
  }
  */

  laws_reset(rb);

  shmem_barrier_all();

  GTC_EXIT(rb);
}


void laws_reset(laws_local_t *rb) {
  GTC_ENTRY();
  laws_global_t *g = rb->g_meta;
  // Reset state to empty
  rb->nlocal = 0;
  rb->head = -1;
  rb->alt_root = 0;
  rb->vtail = 0;
  g->tail   = 0;
  g->split  = 0;
  g->vtail = 0;
  g->max_size = rb->max_size;
  g->elem_size = rb->elem_size;
  g->procid = rb->procid;

  rb->waiting= 0;

  // Save global metadata to root process
  shmem_putmem(rb->gaddr, g, sizeof(laws_global_t), rb->root);
  
  // Reset queue statistics
  rb->nrelease   = 0;
  rb->nreacquire = 0;
  rb->nwaited    = 0;
  rb->nreclaimed = 0;
  GTC_EXIT();
}


void laws_destroy(laws_local_t *rb) {
  GTC_ENTRY();
  shmem_free(rb);
  GTC_EXIT();
}

void laws_print(laws_local_t *rb) {
  GTC_ENTRY();
  laws_global_t *g_meta = rb->g_meta;
  printf("rb: %p {\n", rb);
  printf("   procid  = %d\n", rb->procid);
  printf("   nproc  = %d\n", rb->nproc);
  printf("   nlocal    = %d\n", rb->nlocal);
  printf("   head      = %d\n", laws_head(rb));
  printf("   split     = %d\n", g_meta->split);
  printf("   tail      = %d\n", g_meta->tail);
  printf("   vtail(g)     = %d\n", g_meta->vtail);
  printf("   vtail     = %d\n", rb->vtail);
  printf("   max_size  = %d\n", rb->max_size);
  printf("   elem_size = %d\n", rb->elem_size);
  printf("   local_size = %d\n", laws_local_size(rb));
  printf("   shared_size= %d\n", laws_shared_size(g_meta));
  printf("   public_size= %d\n", laws_public_size(rb));
  printf("   size       = %d\n", laws_size(rb));
  printf("}\n");
  GTC_EXIT();
}

/*==================== STATE QUERIES ====================*/


int laws_head(laws_local_t *rb) {
    int split = rb->g_meta->split;
    return (split + rb->nlocal - 1) % rb->max_size;
}

/*
int laws_split(laws_local_t *rb) {
    int weird_head = (rb->head + 1) % rb->max_size;
    int split = weird_head - rb->nlocal;
    if (split < 0) {
        split = rb->max_size + split;
    }
    return split;
}
*/


int laws_local_isempty(laws_local_t *rb) {
  return rb->nlocal == 0;
}


int laws_shared_isempty(laws_global_t *rb) {
    // how is this going to be checked? Need some way to do this
    // tail is not calculable using only info available in laws_local_t
  return rb->tail == rb->split;
}

int laws_isempty(laws_local_t *rb) {
  return laws_local_isempty(rb) && laws_shared_isempty(rb->g_meta);
}


int laws_local_size(laws_local_t *rb) {
  return rb->nlocal;
}

/*
int laws_reserved_size(laws_local_t *rb) {
    int split = laws_split(rb);
    int public_size = split - rb->vtail;
    if (public_size < 0) {
        public_size = rb->max_size + public_size;
    }
    return public_size + rb->nlocal;
}
*/

int laws_shared_size(laws_global_t *rb) {
  if (laws_shared_isempty(rb)) {// Shared is empty
    return 0;
  }else if (rb->tail < rb->split) {   // No wrap-around
    //printf("dingus1\n");
    return rb->split - rb->tail;
  }else  {                            // Wrap-around
    //printf("dingus2\n");
    return rb->split + rb->max_size - rb->tail;
  }
}


// We don't use this (at least right now...)
int laws_public_size(laws_local_t *rb) {
  laws_global_t *g_meta = rb->g_meta;
  int split = g_meta->split;
  if (rb->vtail == split) {    // Public is empty
    assert (g_meta->tail == rb->vtail && g_meta->tail == split);
    return 0;
  }
  else if (rb->vtail < split)  // No wrap-around
    return split - rb->vtail;
  else                             // Wrap-around
    return split + rb->max_size - rb->vtail;
}


int laws_size(void *b) {
  laws_local_t *rb = (laws_local_t *)b;
  laws_global_t *g_meta = rb->g_meta;
  // update metadata using communication
  // (this function is called rarely, so we shouldn't encounter too much of a performance hit)
  // usually only called after an update getting metadata (gtc_progress), so we should (!) be fine
  return laws_local_size(rb) + laws_shared_size(g_meta);
  //return laws_local_size(rb) + laws_shared_size(
   //       &rb->global[rb->rank_in_node]);
}


/*==================== SYNCHRONIZATION ====================*/


/*
void laws_lock(laws_global_t *rb, int proc) {
  //printf("address of laws_global_t: %p\n", rb);
  //printf("address of lock: %p\n", &rb->lock);
  //printf("proc is %d\n", proc);
  synch_mutex_lock(&rb->lock, proc);
}
*/

void laws_lock(laws_local_t *rb, int proc) {
  //printf("address of laws_global_t: %p\n", rb);
  //printf("address of lock: %p\n", &rb->lock);
  //printf("proc is %d\n", proc);
  synch_mutex_lock(&rb->lock, proc);
}

int laws_trylock(laws_local_t *rb, int proc) {
  return synch_mutex_trylock(&rb->lock, proc);
}


/*
void laws_unlock(laws_global_t *rb, int proc) {
  synch_mutex_unlock(&rb->lock, proc);
}
*/

void laws_unlock(laws_local_t *rb, int proc) {
  synch_mutex_unlock(&rb->lock, proc);
}

/*==================== SPLIT MOVEMENT ====================*/


/*
 * We'll come back to this...
int laws_reclaim_space(laws_local_t *rb) {
    GTC_ENTRY();
    int local_vtail = rb->vtail;
    shmem_getmem();
}
*/

// LAWS queue is locked before this function is run...
int laws_reclaim_space(laws_local_t *rb) {
  GTC_ENTRY();
  int reclaimed = 0;
  int old_vtail = rb->vtail;
  laws_global_t *g_meta = rb->g_meta;

  int g_vtail = g_meta->vtail;
  int tail = g_meta->tail;
  TC_START_TIMER(rb->tc, reclaim);
  //assert(g_vtail <= tail);
  if ((old_vtail != tail) && (g_vtail == tail)) {
    rb->vtail = g_vtail; // Update our vtail to match global
    if (tail > old_vtail) {
      //printf("tail > old_vtail reclaim\n");
      reclaimed = g_vtail - old_vtail;
    }else {
      //printf("else reclaim\n");
      reclaimed = rb->max_size - old_vtail + tail;
    }

    assert(reclaimed > 0);
  }

  // Update our global metadata
  // only do so if we don't reclaim (since we are already communicating in ensure space)

  rb->nreccalls++;
  TC_STOP_TIMER(rb->tc, reclaim);
  GTC_EXIT(reclaimed);
}



void laws_ensure_space(laws_local_t *rb, int n) {
  GTC_ENTRY();
  // Ensure that there is enough free space in the queue.  If there isn't
  // wait until others finish their deferred copies so we can reclaim space.
  int reclaimed;
  TC_START_TIMER(rb->tc, ensure);
  if ((rb->max_size - (laws_local_size(rb) + laws_public_size(rb))) < n) {
    // should we lock here?
    laws_lock(rb, rb->procid);
    {
      /* 
       * Up-to-date version of this would require communication to 
       * update global metadata
       *
       * TODO: Let's worry about this later.
       * Response to TODO: later has come. Also this *should* work now.
       */
      shmem_getmem(rb->g_meta, rb->gaddr, sizeof(laws_global_t), rb->root);
      if ((rb->max_size - laws_size(rb)) < n) {
        // Error: amount of reclaimable space is less than what we need.
        // Try increasing the size of the queue.
        printf("LAWS_SHRB: Error, not enough space in the queue to push %d elements\n", n);
        //laws_print(rb);
        assert(0);
      }
      rb->waiting = 1;
      while ((reclaimed = laws_reclaim_space(rb)) == 0) {
          //printf("dingus alert!\n");
          //laws_print(rb);
          shmem_getmem(rb->g_meta, rb->gaddr, sizeof(laws_global_t), rb->root);
      } /* Busy Wait */ ;
      rb->waiting = 0;
      rb->nwaited++;
    }
    laws_unlock(rb, rb->procid);
  }
  TC_STOP_TIMER(rb->tc, ensure);
  GTC_EXIT();
}



void laws_release(laws_local_t *rb) {
  GTC_ENTRY();
  // Favor placing work in the shared portion -- if there is only one task
  // available this scheme will put it in the shared portion.
  TC_START_TIMER(rb->tc, release);
  // We'll need to fix this; we won't have access to 
  // up-to-date info about the global metadata here
  //
  // Update: we should have something more up-to-date now (gtc_progress)
  if ((laws_local_size(rb)) > 0 && (laws_shared_size(rb->g_meta) == 0)) {
    //printf("before release: \n");
    //laws_print(rb);
  //if (rb->release) {
    int amount  = laws_local_size(rb)/2 + laws_local_size(rb) % 2;
    int split = rb->g_meta->split;
    rb->nlocal -= amount;
    split   = (split + amount) % rb->max_size;
    laws_global_t *g_meta = rb->g_meta;
    g_meta->split = split;
    shmem_putmem(&rb->gaddr->split, &split, sizeof(int), rb->root);
    rb->nrelease++;
    //printf("after release: \n");
    //laws_print(rb);
    gtc_lprintf(DBGSHRB, "release: local size: %d shared size: %d\n", laws_local_size(rb), laws_shared_size(rb->g_meta));
    //printf("release: local size: %d shared size: %d\n", laws_local_size(rb), laws_shared_size(rb->global));
  }
  TC_STOP_TIMER(rb->tc, release);
  GTC_EXIT();
}


void laws_release_all(laws_local_t *rb) {
  GTC_ENTRY();
  int amount  = laws_local_size(rb);
  laws_global_t *g_meta = rb->g_meta;
  rb->nlocal -= amount;
  
  int split = g_meta->split;
  split   = (split + amount) % rb->max_size;
  //printf("release all is releasing all\n");
  shmem_putmem(&rb->gaddr->split, &split, sizeof(int), rb->root);
  g_meta->split = split;
  rb->nrelease++;
  GTC_EXIT();
}


int laws_reacquire(laws_local_t *rb) {
  GTC_ENTRY();
  int amount = 0;

  TC_START_TIMER(rb->tc, reacquire);
  // Favor placing work in the local portion -- if there is only one task
  // available this scheme will put it in the local portion.
  laws_lock(rb, rb->procid);
  
  // Update our view of our global metadata before reacquiring
  shmem_getmem(rb->g_meta, rb->gaddr, sizeof(laws_global_t), rb->root);
  laws_global_t *g_meta = rb->g_meta;
  {
    if (laws_shared_size(rb->g_meta) > laws_local_size(rb)) {
      //printf("laws_shared_size: %d\n", laws_shared_size(rb->g_meta));
      //printf("laws_local_size: %d\n", laws_local_size(rb));
      int split = g_meta->split;
      //printf("before split movement:\n");
      //laws_print(rb);
      int diff    = laws_shared_size(rb->g_meta) - laws_local_size(rb);
      amount      = diff/2 + diff % 2;
      rb->nlocal += amount;
      split = split - amount;
      //printf("after split movement:\n");
      //laws_print(rb);
      if (split < 0) {
        //printf("donkus\n");
        split += rb->max_size;
      }
      g_meta->split = split;
      shmem_putmem(&rb->gaddr->split, &split, sizeof(int), rb->root);
      rb->nreacquire++;
      gtc_lprintf(DBGSHRB, "reacquire: local size: %d shared size: %d\n", laws_local_size(rb), laws_shared_size(g_meta));
    }

    // Assertion: laws_local_isempty(rb) => laws_isempty(rb)
    assert(!laws_local_isempty(rb) || (laws_isempty(rb) && laws_local_isempty(rb)));
  }
  laws_unlock(rb, rb->procid);
  TC_STOP_TIMER(rb->tc, reacquire);

  GTC_EXIT(amount);
}


/*==================== PUSH OPERATIONS ====================*/


static inline void laws_push_n_head_impl(laws_local_t *rb, int proc, void *e, int n, int size) {
  int head, old_head;

  assert(size <= rb->elem_size);
  assert(size == rb->elem_size || n == 1);  // n > 1 ==> size == rb->elem_size
  assert(proc == rb->procid);
  TC_START_TIMER(rb->tc, pushhead);

  // Make sure there is enough space for n elements
  laws_ensure_space(rb, n);

  // Proceed with the push
  old_head = laws_head(rb);
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

void laws_push_head(laws_local_t *rb, int proc, void *e, int size) {
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

  //printf("(%d) pushed head\n", rb->procid);
  GTC_EXIT();
}

void laws_push_n_head(void *b, int proc, void *e, int n) {
  GTC_ENTRY();
  laws_local_t *rb = (laws_local_t *)b;
  laws_push_n_head_impl(rb, proc, e, n, rb->elem_size);
  GTC_EXIT();
}


void *laws_alloc_head(laws_local_t *rb) {
  GTC_ENTRY();
  // Make sure there is enough space for 1 element
  laws_ensure_space(rb, 1);

  rb->nlocal += 1;

  GTC_EXIT(laws_elem_addr(rb, rb->procid, laws_head(rb)));
}

/*==================== POP OPERATIONS ====================*/


int laws_pop_head(void *b, int proc, void *buf) {
  GTC_ENTRY();
  laws_local_t *rb = (laws_local_t *)b;
  int   old_head = 0;
  int   buf_valid = 0;

  assert(proc == rb->procid);

  // If we are out of local work, try to reacquire
  if (laws_local_isempty(rb))
    laws_reacquire(rb);

  if (laws_local_size(rb) > 0) {
    //printf("split: %d\n", rb->g_meta->split);
    //printf("nlocal: %d\n", rb->nlocal);
    old_head = laws_head(rb);

    memcpy(buf, laws_elem_addr(rb, proc, old_head), rb->elem_size);

    rb->nlocal--;
    //printf("split(after): %d\n", rb->g_meta->split);
    //printf("nlocal(after): %d\n", rb->nlocal);
    buf_valid = 1;
  }

  //printf("popped head %d\n", old_head);

  // Assertion: !buf_valid => laws_isempty(rb)
  assert(buf_valid || (!buf_valid && laws_isempty(rb)));

  // printf("(%d) popped head; head num: %d\n", rb->procid, old_head);

  GTC_EXIT(buf_valid);
}


int laws_pop_tail(laws_local_t *rb, int proc, void *buf) {
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

static inline int laws_pop_n_tail_impl(laws_local_t *myrb, int proc, int n, void *e, int steal_vol, int trylock) {
  TC_START_TIMER(myrb->tc, poptail);
  __gtc_marker[1] = 3;
  laws_global_t *trb;
  laws_global_t copy;
  int root;
  int rank;
  int act_proc;
  if (myrb->alt_root) {
      root = proc - (proc % myrb->ncores);
      rank = proc % myrb->ncores;
      act_proc = proc;
  }else {
      root = myrb->root;
      rank = proc;
      act_proc = myrb->global[rank].procid;
      //printf("act_proc: %d\n", act_proc);
  }
  // Attempt to get the lock
  if (trylock) {
    //printf("rank is %d\n", rank);
    //printf("root is %d\n", root);
    //printf("address is %p\n", &myrb->global[rank]);
    if (!laws_trylock(myrb, act_proc)) {
      return -1;
    }
  } else {
    //printf("rank is %d\n", rank);
    //printf("root is %d\n", root);
    //printf("address is %p\n", &myrb->global[rank]);
    laws_lock(myrb, act_proc);
  }

  // Copy the remote RB's metadata
  // Don't need; we already have the metadata
  // Actually: we do need (possibly changed before we acquired lock)
  // Note: in internode steals, myrb->root is redefined to refer to that proc's node's root process
  /*
  laws_global_t *trb;
  laws_global_t copy;
  if (myrb->root == myrb->our_root) {
      shmem_getmem(&myrb->global[proc],  &myrb->global[proc], sizeof(laws_global_t), myrb->root);
      trb = &myrb->global[proc];
  } else {
      shmem_getmem(&copy, &myrb->global[proc], sizeof(laws_global_t), myrb->root);
      trb = &copy;
  }
  */

  shmem_getmem(&copy, &myrb->gaddrs[rank], sizeof(laws_global_t), root);
  trb = &copy;

  // memory address of global metadata (NOTE: copy memory address != actual mem address)
  // keep myself from screwing up mem pointers later on 
  laws_global_t *g_mem = &myrb->gaddrs[rank];


  switch (steal_vol) {
    case STEAL_HALF:
      n = MIN(laws_shared_size(trb)/2 + laws_shared_size(trb) % 2, n);
      break;
    case STEAL_ALL:
      n = MIN(laws_shared_size(trb), n);
      break;
    case STEAL_CHUNK:
      // Get as much as possible up to N.
      n = MIN(n, laws_shared_size(trb));
      break;
    default:
      printf("Error: Unknown steal volume heuristic.\n");
      assert(0);
  }

 // n = laws_shared_size(trb) / 2 + laws_shared_size(trb) % 2;

  // Reserve N elements by advancing the victim's tail
  if (n > 0) {
    int  new_tail;
    // int  metadata;
    int  xfer_size;
    int *loc_addr, *rem_addr;
    //printf("ready to steal from proc %d; tail: %d; split: %d; max_size: %d\n", rank, trb->tail, trb->split, trb->max_size);
    //printf("will steal %d\n", n);

    new_tail    = ((trb)->tail + n) % (trb)->max_size;
    //printf("new_tail is %d\n", new_tail);

    loc_addr    = &new_tail;
    rem_addr    = &g_mem->tail;
    xfer_size   = 1*sizeof(int);

    // Put new tail back to proc 0's global array
    shmem_putmem(rem_addr, loc_addr, xfer_size, root);

    laws_unlock(myrb, act_proc); // Deferred copy unlocks early

    // Transfer work into the local buffer
    if (((trb)->tail + (n-1)) < (trb)->max_size) {    // No need to wrap around

      //printf("steal starting from mem addr: %p\n", laws_elem_addr(trb->local, proc, (trb)->tail));
      // TODO: proc needs to be the actual rank, not the rank in node
      shmem_getmem_nbi(e, laws_elem_addr(myrb, proc, (trb)->tail), n * (trb)->elem_size, trb->procid);    // Store n elems, starting at remote tail, in e
      shmem_quiet();

    } else {    // Need to wrap around
      //printf("split steal is go\n");
      int part_size  = (trb)->max_size - (trb)->tail;

      shmem_getmem_nbi(laws_buff_elem_addr(trb, e, 0), laws_elem_addr(myrb, proc, (trb)->tail), part_size * (trb)->elem_size, trb->procid);

      shmem_getmem_nbi(laws_buff_elem_addr(trb, e, part_size), laws_elem_addr(myrb, proc, 0), (n - part_size) * (trb)->elem_size, trb->procid);

      shmem_quiet();

    }
    /*
    int *steal_from;
    for (int i = 1; i < n + 1; i++) {
        steal_from = (int *)((e + (i * trb->elem_size)) + 8);
        printf("%d, ", *(steal_from - 4));
    }
    printf("\n");
    */

#ifndef LAWS_NODC
    // Accumulate itail_inc onto the victim's intermediate tail
    {
      int itail_inc;
      //long int stride = 1;
      //int count = sizeof(int);

      // How much should we add to the itail?  If we caused a wraparound, we need to also wrap itail.
      // TODO: this might also need to be fixed
      // (You may have changed new_tail just slightly)
      if (new_tail > (trb)->tail)
        itail_inc = n;
      else
        itail_inc = n - (trb)->max_size;
      shmem_atomic_fetch_add(&g_mem->vtail, itail_inc, root);

      shmem_quiet();
    }

#else
    shmem_quiet();
    laws_unlock(myrb, act_proc);
#endif

  } else /* (n <= 0) */ {
    laws_unlock(myrb, act_proc);
  }
  TC_STOP_TIMER(myrb->tc, poptail);
  __gtc_marker[1] = 0;
  return n;
}

int laws_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
  GTC_ENTRY();
  laws_local_t *myrb = (laws_local_t *)b;
  // laws_global_t *global = myrb->global;
  GTC_EXIT(laws_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 0));
}

int laws_try_pop_n_tail(void *b, int proc, int n, void *e, int steal_vol) {
  GTC_ENTRY();
  laws_local_t *myrb = (laws_local_t *)b;
  // laws_global_t *global = myrb->global;
  GTC_EXIT(laws_pop_n_tail_impl(myrb, proc, n, e, steal_vol, 1));
}
