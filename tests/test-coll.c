#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <shmem.h>

#include <tc.h>

#define NITER 100

int main(int argc, char **argv)
{
  unsigned long ulx, uly, ulanswer;
  long lx, ly, lanswer;
  double dx, dy, danswer;
  int ix, iy, ianswer;
  int fails = 0;
  tc_timer_t timer;

  printf("starting run\n"); fflush(stdout);
  gtc_init();

  ulx = _c->rank;
  lx = _c->rank;
  dx = _c->rank;
  ix = _c->rank;

  uly = 0;
  ly = 0;
  dy = 0;
  iy = 0;

  ulanswer = 0;
  lanswer = 0;
  danswer = 0;
  ianswer = 0;

  // correct answer
  for (int i=0; i < _c->size; i++) {
    ulanswer += i;
    lanswer += i;
    danswer += i;
    ianswer += i;
  }

  // test unsigned long reductions
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&ulx, &uly, GtcReduceOpSum, UnsignedLongType, 1);
    if ((_c->rank == 0) && (uly != ulanswer)) 
      fails++;
  }

  eprintf("unsigned long sum reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  shmem_barrier_all();
  fails = 0;

  // test long reductions
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&lx, &ly, GtcReduceOpSum, LongType, 1);
    if ((_c->rank == 0) && (ly != lanswer)) 
      fails++;
  }

  eprintf("long sum reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  shmem_barrier_all();
  fails = 0;

  // test double reductions
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&dx, &dy, GtcReduceOpSum, DoubleType, 1);
    if ((_c->rank == 0) && (dy != danswer)) 
      fails++;
  }

  eprintf("double sum reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  shmem_barrier_all();
  fails = 0;

  // test int reductions
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&ix, &iy, GtcReduceOpSum, IntType, 1);
    if ((_c->rank == 0) && (iy != ianswer)) 
      fails++;
  }

  eprintf("integer sum reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  shmem_barrier_all();
  fails = 0;

  // test int max
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&ix, &iy, GtcReduceOpMax, IntType, 1);
    if ((_c->rank == 0) && (iy != (_c->size - 1))) 
      fails++;
  }

  eprintf("integer max reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  shmem_barrier_all();
  fails = 0;

  // test int max
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&ix, &iy, GtcReduceOpMin, IntType, 1);
    printf("iy: %d\n", iy);
    if ((_c->rank == 0) && (iy != 0)) 
      fails++;
  }


  eprintf("integer min reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  shmem_barrier_all();
  fails = 0;

  shmem_barrier_all();
  gtc_fini();
  return 0;

  // time barriers
  TC_INIT_ATIMER(timer);
  TC_START_ATIMER(timer);
  for (int i=0; i < NITER; i++) {
    // gtc_barrier();
    shmem_barrier_all();
  }
  TC_STOP_ATIMER(timer);
  double max, min, avg, usec;
  usec = TC_READ_ATIMER_USEC(timer);
  gtc_reduce(&usec, &avg, GtcReduceOpSum, DoubleType, 1);
  gtc_reduce(&usec, &max, GtcReduceOpMax, DoubleType, 1);
  gtc_reduce(&usec, &min, GtcReduceOpMin, DoubleType, 1);
  avg /= (double)(_c->size * NITER);
  max /= (double)NITER;
  min /= (double)NITER;
  eprintf("barrier timing: %12.5f us avg / %12.5f us max / %12.5f us min\n", avg, max, min);

  return 0;
}
