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

saws_shrb_t *saws_shrb_create(int elem_size, int max_size) {
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
    rb->completed  = 0;
    rb->steal_val  = 0;
    rb->split      = 0;
    rb->waiting    = 0;

    rb->nrelease   = 0;
    rb->nreacquire = 0;
    rb->nwaited    = 0;
    rb->nreclaimed = 0;

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
    printf("   completed  = %ld\n",  rb->completed);
    printf("}\n");
}

/*================== HELPER FUNCTIONS ===================*/

static inline int initial_steals(uint64_t itasks) {

    uint32_t curr,cnt, total = 0;
    for (cnt = 0, curr = itasks; total != itasks; cnt++) {
        curr = (curr != 1) ? curr >> 1 : 1;
        total += curr;
        curr = itasks - total;
    }
    return cnt;
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


void saws_shrb_reclaim_space(saws_shrb_t *rb) {

    uint64_t steal_val = 0, asteals, isteals, itasks, rtail, valid = 1;
    uint64_t curr_val = shmem_atomic_swap(&rb->steal_val, LOCKQUEUE, rb->procid); // Disable steals

    asteals    = (curr_val >> 39)   & 0x000000001FFFFFF;
    itasks     = (curr_val >> 19)   & 0x00000000007FFFF;
    rtail      = curr_val           & 0x00000000007FFFF;

    isteals    = log2_ceil(itasks);

    uint64_t completed = shmem_atomic_fetch(&rb->completed, rb->procid);

    if (completed == isteals || isteals == 0) {
        rb->completed = 0;
        steal_val |= rb->split; //update tail

        shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);
        rb->tail = rb->split;

    } else {
        steal_val  = asteals << 39;
        steal_val |= valid << 38;
        steal_val |= itasks << 19;
        steal_val |= rtail;

        shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);
        rb->tail = rtail;
    }
    shmem_quiet();

}


void saws_shrb_ensure_space(saws_shrb_t *rb, int n) {
    // Ensure that there is enough free space in the queue.  If there isn't
    // wait until others finish their deferred copies so we can reclaim space.
    if (rb->max_size - (saws_shrb_local_size(rb) + saws_shrb_public_size(rb)) < n) {
        saws_shrb_lock(rb, rb->procid);
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
        saws_shrb_unlock(rb, rb->procid);
    }
}


void saws_shrb_release(saws_shrb_t *rb) {

    if (rb->nlocal > 0 && (saws_shrb_shared_size(rb) == 0)) {
        int nshared  = rb->nlocal / 2 + rb->nlocal % 2;

        rb->nlocal  -= nshared;
        rb->split    = (rb->split + nshared) % rb->max_size;

        gtc_lprintf(DBGSHRB, "releasing %d tasks\n", nshared);
        uint64_t steals  = log2_ceil(nshared);

        uint64_t  steal_val, valid = 1;
        steal_val   = steals << 39;
        steal_val    |= valid << 38;
        steal_val    |= nshared << 19;
        steal_val    |= rb->tail;

        rb->completed = 0;
        rb->nrelease++;

        shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);

    } //assert (rb->tail >= 0 && rb->tail < rb->max_size);
}


void saws_shrb_release_all(saws_shrb_t *rb) {

    // this function doesn't really work.
    int amount  = saws_shrb_local_size(rb);
    rb->nlocal -= amount;
    rb->split   = (rb->split + amount) % rb->max_size;
    uint64_t asteals = log2_ceil(amount + saws_shrb_shared_size(rb)) - 1;

    unsigned long val = asteals << 39;
    uint64_t valid = 1;
    valid = valid << 38;
    val    |= valid;
    val |= amount << 19;
    val |= rb->tail;

    shmem_atomic_set(&rb->steal_val, val, rb->procid);
    rb->nrelease++;
}


void saws_shrb_reacquire(saws_shrb_t *rb) {

    if ((saws_shrb_shared_size(rb) > rb->nlocal) && (rb->nlocal == 0)) {

        int ntasks, stolen, nlocal = 0;
        uint64_t steal_val, asteals, isteals, itasks, rtail, nsteals, valid = 1;

        unsigned long curr_val = shmem_atomic_swap(&rb->steal_val, LOCKQUEUE, rb->procid);  // Disable steals

        shmem_quiet();

        asteals = curr_val >> 39;
        itasks = (curr_val >> 19) & 0x00000000007FFFF;
        rtail = curr_val & 0x00000000007FFFF;

        isteals = log2_ceil(itasks);

        // all tasks have already been stolen or are in progress
        if (asteals > isteals || asteals <= 0)
            return;

        // calculate amount of tasks aleady stolen
        ntasks = itasks;
        stolen = 0;
        uint64_t tasks_left = itasks;
        for (uint32_t i = isteals; i > asteals; i--) {
            stolen += (ntasks >> 1) + ntasks % 2;
            ntasks = ntasks >> 1;
        }
        tasks_left -= stolen;
        nlocal = ((tasks_left >> 1) + tasks_left % 2);
        tasks_left -= nlocal;

        gtc_lprintf(DBGSHRB, "reacquiring %d out of %ld remaining tasks.\n" , nlocal, tasks_left + nlocal);

        rb->nlocal += nlocal;
        rb->split = rb->split - nlocal;
        //assert(rb->split >= 0 && rb->split < rb->max_size);

        nsteals      = log2_ceil(tasks_left);

        steal_val    = nsteals << 39;
        steal_val   |= valid << 38;
        steal_val   |= tasks_left << 19;
        steal_val   |= rtail;

        rb->completed = 0;
        shmem_atomic_set(&rb->steal_val, steal_val, rb->procid);

        shmem_quiet();
    }
    assert(!saws_shrb_local_isempty(rb) || (saws_shrb_isempty(rb) && saws_shrb_local_isempty(rb)));

}


/*==================== PUSH OPERATIONS ====================*/


static inline void saws_shrb_push_n_head_impl(saws_shrb_t *rb, int proc, void *e, int n, int size) {
    int head, old_head;

    assert(size <= rb->elem_size);
    assert(size == rb->elem_size || n == 1);  // n > 1 ==> size == rb->elem_size
    assert(proc == rb->procid);

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

    int valid, rtail, ntasks = 0, stolen = 0;
    uint64_t steal_val, asteals, isteals, itasks, decriment = -1;
    decriment = decriment << 39;

    test:

    if (myrb->targets[proc] == FullQueue)
        steal_val = shmem_atomic_fetch_add(&myrb->steal_val, decriment, proc);
    else
        steal_val = shmem_atomic_fetch(&myrb->steal_val, proc);

    asteals    =    (steal_val >> 39)  & 0x000000001FFFFFF;
    valid      =    (steal_val >> 38)  & 0x000000000000001;
    itasks     =    (steal_val >> 19)  & 0x000000000007FFFF;
    rtail      =    steal_val          & 0x000000000007FFFF;

    isteals    =    log2_ceil(itasks);

    // queue is busy
    if (!valid)
        return 0;

    if (asteals > isteals || asteals <= 0) {
        myrb->targets[proc] = EmptyQueue;
        return 0;
    } else if (myrb->targets[proc] == EmptyQueue) {
        myrb->targets[proc] = FullQueue;
        goto test;
    }


   // gtc_lprintf(DBGSHRB, "Calculating steal volume, isteals %ld, asteals %ld itasks %ld\n", isteals, asteals, itasks);

    // calculate steal volume
    ntasks = itasks;
    stolen = 0;
    int remaining = itasks;
    for (ntasks = itasks; isteals > asteals; isteals--) {
        remaining = ntasks >> 1;
        ntasks = (ntasks >> 1) + ntasks % 2;
        stolen += ntasks;
        ntasks = remaining;
    }
    ntasks = (itasks - stolen) / 2 + (itasks - stolen) % 2;

    gtc_lprintf(DBGSHRB, "stealing %d tasks from (%d), starting at index %d\n", ntasks, proc, rtail + stolen);

    void* rptr = &myrb->q[0] + ((rtail + stolen) * myrb->elem_size);

    if (rtail + ntasks < myrb->max_size) { // No wrap
        shmem_getmem_nbi(e, rptr, ntasks * myrb->elem_size, proc);

    } else { // wrap
        int part_size = myrb->max_size - myrb->tail;
        //printf("part size: %d\n", part_size);

        shmem_getmem_nbi(saws_shrb_buff_elem_addr(myrb, e, 0), rptr, part_size * myrb->elem_size, proc);
        shmem_getmem_nbi(saws_shrb_buff_elem_addr(myrb, e, part_size),
                         saws_shrb_elem_addr(myrb, proc, 0),
                         (ntasks - part_size) * myrb->elem_size, proc); // Wrap
    }

    shmem_atomic_inc(&myrb->completed, proc);
    shmem_quiet();
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
