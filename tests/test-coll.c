#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

#include <tc.h>

#define NITER 10000

int main(int argc, char **argv)
{
  unsigned long x, y, answer, fails = 0;
  tc_timer_t timer;

  printf("starting run\n"); fflush(stdout);
  gtc_init();

  x = _c->rank;
  y = 0;
  answer = 0;

  // correct answer
  for (int i=0; i < _c->size; i++)
    answer += i;

  for (int i=0; i < _c->size; i++) {
    if (i == _c->rank) 
      printf("%d ",_c->rank); fflush(stdout);
    gtc_barrier();
  }
  eprintf("\n"); fflush(stdout);

  // test reductions
  for (int i=0; i<NITER; i++) {
    gtc_reduce(&x, &y, GtcReduceOpSum, UnsignedLongType, 1);
    if ((_c->rank == 0) && (y != answer)) 
      fails++;
  }
  eprintf("reduction tests: %s\n", (fails == 0) ? "passed" : "failed");
  gtc_barrier();

  for (int i=0; i<NITER; i++) {
    x = 42;
    gtc_broadcast(&x, UnsignedLongType, 1);
    if (x != 42) fails++;
  }

  gtc_reduce(&fails, &y, GtcReduceOpSum, UnsignedLongType, 1);
  eprintf("broadcast tests: %s\n", (y == 0) ? "passed" : "failed");
  gtc_barrier();

  // time barriers
  TC_INIT_ATIMER(timer);
  TC_START_ATIMER(timer);
  for (int i=0; i < NITER; i++) {
    gtc_barrier();
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
