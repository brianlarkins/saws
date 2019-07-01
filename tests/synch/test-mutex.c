#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

#include <tc.h>
#include <mutex.h>

#define NITER 1000

int mythread, nthreads, rankno;

int main(int argc, char **argv) {
  tc_timer_t timer;
  synch_mutex_t mutex;

  shmem_init();
  
  synch_mutex_init(&mutex);

  if (shmem_my_pe() == 0)
    printf("Mutex test starting on %d processes\n", shmem_n_pes());fflush(stdout);

  shmem_barrier_all();

  TC_INIT_ATIMER(timer);
  TC_START_ATIMER(timer);

#if 0
  printf("%d: init  %"PRId64"\n", _c->rank, *(int64_t *)mutex.lockval);fflush(stdout);

  gtc_barrier();

  if (_c->rank == 0) {
    synch_mutex_lock(&mutex, 0);
    synch_mutex_lock(&mutex, 1);
  }

  gtc_barrier();
  printf("%d: locked  %"PRId64"\n", _c->rank, *(int64_t *)mutex.lockval);fflush(stdout);

  if (_c->rank == 0) {
    sleep(1);
  } else {
    synch_mutex_lock(&mutex, 0);
  }


  if (_c->rank == 0) {
    synch_mutex_unlock(&mutex, 0);
    synch_mutex_unlock(&mutex, 1);
  } else {
    synch_mutex_unlock(&mutex, 0);
  }

  gtc_barrier();
  printf("%d: unlocked %"PRId64"\n", _c->rank, *(int64_t *)mutex.lockval);fflush(stdout);
  exit(0);
#endif


  for (int i = 0; i < NITER; i++) {
    for (int j = 0; j < shmem_n_pes(); j++) {
      //printf("%d: locking\n", _c->rank);
      synch_mutex_lock(&mutex, j);
      //printf("%d: done locking\n", _c->rank);
      // Critical section
      usleep(1000 + (rand()%10));
      //sleep(1 + (rand()%3));
      //printf("%d: unlocking\n", _c->rank);
      synch_mutex_unlock(&mutex, j);
      //printf("%d: done unlocking\n", _c->rank);
    }
  }

  shmem_barrier_all();
  TC_STOP_ATIMER(timer);

  if (shmem_my_pe() == 0)
    printf("Mutex test completed %d mutex ops in %f sec\n", NITER*shmem_n_pes(), TC_READ_ATIMER_SEC(timer));

  shmem_finalize();
  return 0;
}
