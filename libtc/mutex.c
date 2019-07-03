#include <stdio.h>
#include <assert.h>

#include "tc.h"
#include "mutex.h"

#define SYNCH_MUTEX_LOCKED   1
#define SYNCH_MUTEX_UNLOCKED 0

#define LINEAR_BACKOFF

#define USE_SHMEM_LOCKS


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
  m->locks = shmem_calloc(shmem_n_pes(), sizeof(long));
}



/** Lock the given mutex on the given processor.  Blocks until mutex is acquired.
  *
  *  @param[in] lock Array that holds current lock state for every processor.
  *  @param[in] proc Processor id to index into lock array.
  */
void synch_mutex_lock(synch_mutex_t *m, int proc) {

#ifdef USING_SHMEM_LOCKS
  printf("processor %d in array before set lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
  shmem_set_lock(&m->locks[proc]);
  printf("processor %d in array after set lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
#endif /* USING_SHMEM_LOCKS */

}



/** Attempt to lock the given mutex on the given processor.
  *
  *  @param[in] lock Array that holds current lock state for every processor. 
  *  @param[in] proc Processor id to index into lock array.
  *  @return         0 if lock set successfully, 1 if lock already set by other call.
  */
int synch_mutex_trylock(synch_mutex_t *m, int proc) {
  int ret = -1;

#ifdef USING_SHMEM_LOCKS
  ret = shmem_test_lock(&m->locks[proc]);
#endif /*  USING_SHMEM_LOCKS */

  return ret;  
}


/** Unlock the given mutex on the given processor.
  *
  * @param[in] lock Array that holds current lock state for every processor.
  * @param[in] proc Processor id to index into lock array.
  */
void synch_mutex_unlock(synch_mutex_t *m, int proc) {

#ifdef USING_SHMEM_LOCKS
  printf("processor %d in array before clear lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
  shmem_clear_lock(&m->locks[proc]);
  printf("processor %d in array after clear lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
#endif /* USING_SHMEM_LOCKS */

}
