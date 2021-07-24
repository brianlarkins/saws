/*********************************************************/
/*                                                       */
/*  mutex.c - scioto openshmem mutex lock implementation */
/*    (c) 2021 see COPYRIGHT in top-level                */
/*                                                       */
/*********************************************************/
#include <stdio.h>
#include <assert.h>

#include "tc.h"
#include "mutex.h"

#define SYNCH_MUTEX_LOCKED   1
#define SYNCH_MUTEX_UNLOCKED 0

#define LINEAR_BACKOFF

//#define USING_SHMEM_LOCKS

#ifdef LINEAR_BACKOFF
#define SPINCOUNT 1000
#define MAXSPIN   100000
double synch_mutex_dummy_work = 0.0;
#endif /* LINEAR_BACKOFF */

// Stats to track performance of locking
double        synch_mutex_lock_nattempts_squares = 0; // Use to calculate variance as the of the squares
                                                      // minus the square of the mean
unsigned long synch_mutex_lock_nattempts_last    = 0;
unsigned long synch_mutex_lock_nattempts_sum     = 0;
unsigned long synch_mutex_lock_nattempts_max     = 0;
unsigned long synch_mutex_lock_nattempts_min     = 0;
unsigned long synch_mutex_lock_ncalls_contention = 0;
unsigned long synch_mutex_lock_ncalls            = 0;


/** Initialize a local mutex.
  *
  *  @param[in] lock Array that holds current lock state for every processor.
  *  NOTE: This must be a collective call
  */
void synch_mutex_init(synch_mutex_t *m) {
  GTC_ENTRY();
  m->locks = gtc_shmem_calloc(shmem_n_pes(), sizeof(long));
  GTC_EXIT();
}



/** Lock the given mutex on the given processor.  Blocks until mutex is acquired.
  *
  *  @param[in] lock Array that holds current lock state for every processor.
  *  @param[in] proc Processor id to index into lock array.
  */
void synch_mutex_lock(synch_mutex_t *m, int proc) {
  GTC_ENTRY();
  int nattempts = 0, backoff;
  volatile long lock_val = SYNCH_MUTEX_UNLOCKED;

  gtc_lprintf(DBGSYNCH, "synch_mutex_lock (%p, %d)\n", m, proc);

#ifdef USING_SHMEM_LOCKS

  UNUSED(lock_val);
  UNUSED(nattempts);
  UNUSED(backoff);

  shmem_set_lock(&m->locks[proc]);

#else /* !USING_SHMEM_LOCKS */

  do {

    lock_val = shmem_atomic_swap(&m->locks[proc], SYNCH_MUTEX_LOCKED, proc);

#ifdef LINEAR_BACKOFF
    // Linear backoff to avoid flooding the network and bogging down the
    // remote data server.
    backoff = _c->rank == proc ?  0 : MIN(SPINCOUNT*nattempts, MAXSPIN);
    for (int i = 0; i < backoff; i++) {
      synch_mutex_dummy_work += 1.0;
    }
#endif /* LINEAR_BACKOFF */
    nattempts++;

  } while (lock_val != SYNCH_MUTEX_UNLOCKED);

#endif /* USING_SHMEM_LOCKS */
  GTC_EXIT();
}



/** Attempt to lock the given mutex on the given processor.
  *
  *  @param[in] lock Array that holds current lock state for every processor.
  *  @param[in] proc Processor id to index into lock array.
  *  @return         0 if lock set successfully, 1 if lock already set by other call.
  */
int synch_mutex_trylock(synch_mutex_t *m, int proc) {
  int ret = -1;
  long lock_val;

  gtc_lprintf(DBGSYNCH, "synch_mutex_trylock (%p, %d)\n", m, proc);

#ifdef USING_SHMEM_LOCKS
  UNUSED(lock_val);
  ret = shmem_test_lock(&m->locks[proc]);
#else  /* !USING_SHMEM_LOCKS */

    lock_val = shmem_atomic_swap(&m->locks[proc], SYNCH_MUTEX_LOCKED, proc);
    ret = (lock_val == SYNCH_MUTEX_UNLOCKED);

#endif /*  USING_SHMEM_LOCKS */

  return ret;
}


/** Unlock the given mutex on the given processor.
  *
  * @param[in] lock Array that holds current lock state for every processor.
  * @param[in] proc Processor id to index into lock array.
  */
void synch_mutex_unlock(synch_mutex_t *m, int proc) {
  GTC_ENTRY();
  gtc_lprintf(DBGSYNCH, "synch_mutex_unlock (%p, %d)\n", m, proc);

#ifdef USING_SHMEM_LOCKS

  shmem_clear_lock(&m->locks[proc]);

#else  /* !USING_SHMEM_LOCKS */

  shmem_atomic_set(&m->locks[proc], SYNCH_MUTEX_UNLOCKED, proc);

#endif /* USING_SHMEM_LOCKS */
  GTC_EXIT();
}
