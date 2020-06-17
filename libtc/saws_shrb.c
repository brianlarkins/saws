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

#define STEAL_DECRIMENT 0xffffffffff000000 
/**
 * Portals Steal-N Queue Implementation
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
 *
 * A Portals ME is created for each group of N tasks, where N is defined at creation
 *
 * The ring buffer can in in several states:
 *
 * 1. Empty.  In this case we have nlocal == 0, tail == split, and vtail == itail == tail
 *
 * 2. No Wrap-around: (N = 3)
 *
 *              ME1 ME2
 *               v  v
 *  _______________________________________________________________
 * |            |$$$$$$|/////|                                     |
 * |____________|$$$$$$|/////|_____________________________________|
 *              ^      ^     ^
 *             tail  split  head
 *
 * $ -- Available elements in the shared portion of the queue
 * / -- Reserved elements in the local portion of the queue
 *   -- Free space
 *
 * 2. Wrapped-around: (N = 5)
 *
 *                                   ME1  ME2  ME3  ME4  ME5
 *                                   v    v    v    v    v
 *  _______________________________________________________________
 * |/////////|                      |$$$$$$$$$$$$$$$$$$$$$$$$$|////|
 * |/////////|______________________|$$$$$$$$$$$$$$$$$$$$$$$$$|////|
 *           ^                      ^                         ^
 *         head                    tail                     split
 */

saws_shrb_t *saws_shrb_create(int elem_size, int max_size) {

    saws_shrb_t  *rb;
    saws_shrb_t **rbs;
    int procid, nproc;

    setbuf(stdout, NULL);

    procid = shmem_my_pe();
    nproc = shmem_n_pes();

    gtc_lprintf(DBGSHRB, "  Thread %d: saws_shrb_create()\n", procid);

    // Allocate the struct and the buffer contiguously in shared space
    rbs = malloc(sizeof(saws_shrb_t*) * nproc);
    assert(rbs != NULL);
    rbs[procid] = shmem_malloc(sizeof(saws_shrb_t) + elem_size*max_size);

    rb = rbs[procid];

    rb->procid  = procid;
    rb->nproc  = nproc;
    rb->elem_size = elem_size;
    rb->max_size  = max_size;
    rb->rbs       = rbs;
    saws_shrb_reset(rb);

    // Initialize the lock
    synch_mutex_init(&rb->lock);

    shmem_barrier_all();

    return rb;
}


void saws_shrb_reset(saws_shrb_t *rb) {
    // Reset state to empty
    rb->nlocal    = 0;
    rb->tail      = 0;
    rb->completed = 0;
    rb->steal_val = 0;
    rb->itail     = 0;
    rb->vtail     = 0;
    rb->split     = 0;

    rb->waiting   = 0;

    // Reset queue statistics
    rb->nrelease   = 0;
    rb->nreacquire = 0;
    rb->nwaited    = 0;
    rb->nreclaimed = 0;
}


void saws_shrb_destroy(saws_shrb_t *rb) {
    free(rb->rbs);
    shmem_free(rb);
}


void saws_shrb_print(saws_shrb_t *rb) {
    printf("rb: %p {\n", rb);
    printf("   procid  = %d\n", rb->procid);
    printf("   nproc  = %d\n", rb->nproc);
    printf("   nlocal    = %d\n", rb->nlocal);
    printf("   head      = %d\n", saws_shrb_head(rb));
    printf("   split     = %d\n", rb->split);
    printf("   tail      = %ld\n", rb->tail);
    printf("   max_size  = %d\n", rb->max_size);
    printf("   elem_size = %d\n", rb->elem_size);
    printf("   local_size = %d\n", saws_shrb_local_size(rb));
    printf("   shared_size= %d\n", saws_shrb_shared_size(rb));
    printf("   public_size= %d\n", saws_shrb_public_size(rb));
    printf("   size       = %d\n", saws_shrb_size(rb));
    printf("   a_steals   = %ld\n", (rb->steal_val >> 24));
    printf("   i_steals   = %ld\n", ((rb->steal_val >> 19) & 0x1F));
    printf("   completed  = %d\n",  rb->completed);
    printf("}\n");
}


int log_base2(long val) {

    int newval = log10(val) / log10(2) + 1;
    return newval;

}

//prints binary representation of an integer
void itobin(int v) {
    unsigned int mask=1<<((sizeof(int)<<3)-1);
    while(mask) {
        printf("%d", (v&mask ? 1 : 0));
        mask >>= 1;
    }
}

/*
void tests_decriment(saws_shrb_t *rb, int n) {

    int isteals;
    int  
        long val = shmem_atomic_fetch_add(&myrb->steal_val, STEAL_DECRIMENT, rb->procid);

    isteals = ((val >> 19) & 0x1F);     // 5 bits, shifted
    assert(isteals == n)
        //int rtail = val & 0x000000000007FFFF; // Low 19 bits of val
        int asteals = (oldval >> 24);
    int a = shmem_atomic_fetch(&myrb->steal_val, proc);
    printf("\n old %d new %d\n",asteals, a >> 24);
    int nshared  = saws_shrb_local_size(rb)/2 + saws_shrb_local_size(rb) % 2;
    rb->split    = (rb->split + nshared) % rb->max_size;
    long val     = asteals << 24;
    val         |= asteals << 19;
    val         |= rb->tail;

}*/

/*==================== STATE QUERIES ====================*/


int saws_shrb_head(saws_shrb_t *rb) {
    return (rb->split + rb->nlocal - 1) % rb->max_size;
}


int saws_shrb_local_isempty(saws_shrb_t *rb) {
    return rb->nlocal == 0;
}


int saws_shrb_shared_isempty(saws_shrb_t *rb) {
    return rb->tail == rb->split || (rb->steal_val >> 24 == 0);
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
    if (rb->tail == rb->split) {    // Public is empty
        //assert (rb->tail == rb->itail && rb->tail == rb->split);
        return 0;
    }
    else if (rb->tail < rb->split)  // No wrap-around
        return rb->split - rb->tail;
    else                             // Wrap-around
        return rb->split + rb->max_size - rb->tail;
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

    static uint64_t val = -1;
    int curr_val = shmem_atomic_swap(&rb->tail, val, rb->procid); // Disable steals
    
    long rtail   = curr_val & 0x000000000007FFF; // Low 19 bits of val
    long asteals = (rb->steal_val >> 24);
    long isteals = (rb->steal_val >> 19) & 0x1F;

    // shared tasks have not been completed and thus cannot be reclaimed
    if (pow(2, asteals - 1) == rb->split - rtail) {
        rb->tail = rtail; 
        return;
    }

    if ((isteals - asteals) != shmem_atomic_fetch(&rb->completed, rb->procid)) 
        ;
    
    //  THIS PROBABLY NEEDS TO BE FIXED FOR asteals < rb->completed
    rb->tail = rtail + pow(2, abs(asteals - rb->completed));
    if (asteals == 0) rb->tail = rtail+1;

    //if(rb->procid == 0){  printf("queue after reclaim\n"); saws_shrb_print(rb);}

    rb->nreclaimed++;
}


void saws_shrb_ensure_space(saws_shrb_t *rb, int n) {
    // Ensure that there is enough free space in the queue.  If there isn't
    // wait until others finish their deferred copies so we can reclaim space.
    if (rb->max_size - (saws_shrb_local_size(rb) + saws_shrb_public_size(rb)) < n) {
        saws_shrb_lock(rb, rb->procid);
        {
            if (rb->max_size - saws_shrb_size(rb) < n) {
                // Error: amount of reclaimable space is less than what we need.
                // Try increasing the size of the queue.
                printf("SDC_SHRB: Error, not enough space in the queue to push %d elements\n", n);
                saws_shrb_print(rb);
                assert(0);
            }
            rb->waiting = 1;
            // while (saws_shrb_reclaim_space(rb) == 0) /* Busy Wait */ ; // CHECK reclaimm return
            rb->waiting = 0;
            rb->nwaited++;
        }
        saws_shrb_unlock(rb, rb->procid);
    }
}


void saws_shrb_release(saws_shrb_t *rb) {

    // Favor placing work in the shared portion -- if there is only one task
    // available this scheme will put it in the shared portion.
    if (saws_shrb_local_size(rb) > 0 && (saws_shrb_shared_size(rb) == 0)) {
        int nshared  = saws_shrb_local_size(rb)/2 + saws_shrb_local_size(rb) % 2;
        rb->nlocal  -= nshared;
        rb->split    = (rb->split + nshared) % rb->max_size;
        int asteals  = log_base2(nshared);
        
        //if ((nshared % 2 != 0 && nshared != 1) || (ceil(log2(nshared) != floor(log2(nshared))))) asteals++;
        long val     = asteals << 24;
        val         |= asteals << 19;
        val         |= rb->tail;
        rb->nrelease++;

        shmem_atomic_swap(&rb->steal_val, val, rb->procid);
    }
}


void saws_shrb_release_all(saws_shrb_t *rb) {
    int amount  = saws_shrb_local_size(rb);
    rb->nlocal -= amount;
    rb->split   = (rb->split + amount) % rb->max_size;
    //only works when shared_size is 0
    int asteals = log_base2(amount + saws_shrb_shared_size(rb)) - 1;

    long val = asteals << 24;
    val |= asteals << 19;
    val |= rb->tail;

    shmem_atomic_swap(&rb->steal_val, val, rb->procid);
    rb->nrelease++;
}


void saws_shrb_reacquire(saws_shrb_t *rb) {

    // Favor placing work in the local portion -- if there is only one task
    // available this scheme will put it in the local portion.
    int nlocal = 0;
    static long mytail = -1;
    int curr_val = shmem_atomic_swap(&rb->steal_val, mytail, rb->procid);  // Disable steals

    if (saws_shrb_shared_size(rb) > saws_shrb_local_size(rb)) {
        mytail = curr_val;
        mytail &= 0x00000000007FFFF; // Low 19 bits of val

        nlocal = pow(2, (curr_val >> 24) - 1) / 2; //number of tasks to reclaim
        if (saws_shrb_shared_size(rb) ==1) nlocal = 1; //edge case

        rb->nlocal += nlocal;
        rb->split = rb->split - nlocal;

        int nsteals    = log_base2(saws_shrb_shared_size(rb));
        rb->completed = 0;
        long val       = nsteals << 24;
        val           |= nsteals << 19;
        val           |= mytail;

        shmem_atomic_swap(&rb->steal_val, val, rb->procid);
    }
   // if (rb->procid == 0) saws_shrb_print(rb);
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
    // saws_shrb_push_n_head_impl(rb, proc, e, 1, size);
    int old_head;

    assert(size <= rb->elem_size);
    assert(proc == rb->procid);

    // Make sure there is enough space for n elements
    saws_shrb_ensure_space(rb, 1);

    // Proceed with the push
    old_head    = saws_shrb_head(rb);
    rb->nlocal += 1;

    memcpy(saws_shrb_elem_addr(rb, proc, (old_head+1)%rb->max_size), e, size);

    // printf("(%d) pushed head\n", rb->procid);
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

    // Assertion: !buf_valid => saws_shrb_isempty(rb)
    assert(buf_valid || (!buf_valid && saws_shrb_isempty(rb)));

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

    saws_shrb_t trb;
    // Copy the remote RB's metadata
    shmem_getmem(&trb, myrb, sizeof(saws_shrb_t), proc);

    switch (steal_vol) {
        case STEAL_HALF:
            n = MIN(saws_shrb_shared_size(&trb)/2 + saws_shrb_shared_size(&trb) % 2, n);
            break;
        case STEAL_ALL:
            n = MIN(saws_shrb_shared_size(&trb), n);
            break;
        case STEAL_CHUNK:
            n = MIN(n, saws_shrb_shared_size(&trb));
            break;
        default:
            printf("Error: Unknown steal volume heuristic.\n");
            assert(0);
    }


    int ntasks = 0;
    if (n > 0) {//runs as long as there are tasks to steal
        //static uint64_t val = -1;
        //val = val << 24;
        //printf("steal_decriment %ld", STEAL_DECRIMENT);
        long oldval = shmem_atomic_fetch_add(&myrb->steal_val, STEAL_DECRIMENT, proc);

        int rtail = oldval & 0x000000000007FFFF; // Low 19 bits of val
        int isteals = ((oldval >> 19) & 0x1F);     // 5 bits, shifted
        int asteals = (oldval >> 24);
        //int a = shmem_atomic_fetch(&myrb->steal_val, proc);
        //printf("\n old %d new %d\n",asteals, a >> 24);
        if ((rtail < 0) || (asteals < 0))
            return 0;
        //if ((asteals == 0)) 
        //    shmem_atomic_swap(&myrb->tail, trb.split, proc);

        ntasks = pow(2, asteals - 1) / 2; // Compute steal volume
        if (saws_shrb_shared_size(&trb) % 2 != 0) ntasks = ntasks / 2;
        //else if (ceil(log2(saws_shrb_shared_size(&trb)) != floor(log2(saws_shrb_shared_size(&trb))))) ntasks = ntasks / 2;
        //needed to steal last task. may cause a race condition.
        if (ntasks == 0 && trb.completed != isteals) ntasks = 1;

        //calculate index of first task of steal
        int start = 0;
        for (int i = isteals; i > asteals; i--) {
            start += pow(2, (i - 1)) / 2;
        }
        if ((&trb)->tail + (ntasks-1) < (&trb)->max_size) { // No need to wrap around
            void* rptr = myrb->q + (rtail + (start * trb.elem_size));
            shmem_getmem_nbi(e, rptr/*saws_shrb_elem_addr(myrb, proc, (&trb)->tail)*/, ntasks * (&trb)->elem_size, proc);

        } else { // Need to wrap around

            int part_size = (&trb)->max_size - (&trb)->tail;

            void* rptr = myrb->q + (rtail + (start * trb.elem_size));

            shmem_getmem_nbi(saws_shrb_buff_elem_addr(&trb, e, 0), rptr, part_size * (&trb)->elem_size, proc); // Grab tasks up until end of trb.
           shmem_getmem_nbi(saws_shrb_buff_elem_addr(&trb, e, part_size), saws_shrb_elem_addr(myrb, proc, 0), (ntasks - part_size) * (&trb)->elem_size, proc); // Wrap
        }
        //if (trb.completed + 1 == isteals)
          //  shmem_atomic_swap(&myrb->tail, trb.split, proc);

        shmem_atomic_inc(&myrb->completed, proc);
    }

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
