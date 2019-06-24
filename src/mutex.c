#include <stdio.h>
#include <assert.h>
#include <shmem.h>

#include "mutex.h"
#include "debug.h"

#define MAX(X,Y) ((X)>(Y)?(X):(Y))
#define MIN(X,Y) ((X)<(Y)?(X):(Y))

#define SYNCH_MUTEX_LOCKED   1
#define SYNCH_MUTEX_UNLOCKED 0

#define LINEAR_BACKOFF

#ifdef USING_ARMCI_MUTEXES
// Use ARMCI's built in mutexes rather than RMW mutexes Note: We don't free
// ARMCI mutexes at shutdown, this should be ok but could cause problems if an
// implementation requires mutexes be freed.
#define NUM_ARMCI_MUTEXES 100
static int next_free_armci_mutex = -1;
#endif


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
  *  @param[in] proc Processor id to index into lock array.
  */
void synch_mutex_init(long *lock, int proc) {
  // printf("(%d) hello!\n", proc);
  lock[proc] = 0;
}


/** Lock the given mutex on the given processor.  Blocks until mutex is acquired.
  *
  *  @param[in] lock Array that holds current lock state for every processor.
  *  @param[in] proc Processor id to index into lock array.
  */

// ** EDIT ** Not sure if we need this:

/* #ifdef LINEAR_BACKOFF
 * #define SPINCOUNT 1000
 * #define MAXSPIN   100000
 * double synch_mutex_dummy_work = 0.0;
 * #endif LINEAR_BACKOFF
 */

void synch_mutex_lock(long *lock, int proc) {
  printf("processor %d in array before set lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
  shmem_set_lock(&lock[proc]);
  printf("processor %d in array after set lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
}


/** Attempt to lock the given mutex on the given processor.
  *
  *  @param[in] lock Array that holds current lock state for every processor. 
  *  @param[in] proc Processor id to index into lock array.
  *  @return         0 if lock set successfully, 1 if lock already set by other call.
  */
int synch_mutex_trylock(long *lock, int proc) {
  int ret = -1;

  ret = shmem_test_lock(&lock[proc]);

  return ret;  
}


/** Unlock the given mutex on the given processor.
  *
  * @param[in] lock Array that holds current lock state for every processor.
  * @param[in] proc Processor id to index into lock array.
  */
void synch_mutex_unlock(long *lock, int proc) {
  printf("processor %d in array before clear lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
  shmem_clear_lock(&lock[proc]);
  printf("processor %d in array after clear lock is %ld at %p\n", proc, lock[proc], &lock[proc]);
}
