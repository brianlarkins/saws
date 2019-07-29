#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>

#include "tc.h"

double wctime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec + 1E-6 * tv.tv_usec);
}


double busy_wait_work = 0.0;
void busy_wait(int busy_iter) {
  int i;
  
  for (i = 0; i < busy_iter; i++)
    busy_wait_work += 1.0;
}


unsigned long tune_busy_wait(double desired_time) {
  int    i, max_iter = 100;
  int    busy_iter = 10000;
  double time     = 0.0;
  double thresh   = 0.0001;

  for (i = 1; fabs(time - desired_time) > thresh && i <= max_iter; i++) {
    time = wctime();
    busy_wait(busy_iter);
    time = wctime() - time;

    //printf("%d: time = %f, desired_time = %f, ratio = %f\n", busy_iter, time, desired_time, desired_time/time);
    //busy_iter = (int) (((double) busy_iter) * (desired_time/time));  // Direct
    busy_iter += (int) ((((double) busy_iter) * (desired_time/time) - busy_iter) * (2.0/i)); // Gradient descent
  }

  return busy_iter;
}

#ifdef BW_MAIN
int main(int argc, char **argv) {
  unsigned long  niter;
  double work_time = 0.001, t1, t2;
  tc_timer_t check;

  niter = tune_busy_wait(work_time);

  printf("TIME=%fs, NITER=%lu, MFLOPS = %f\n", work_time, niter, niter/work_time/1.0e6);
  TC_INIT_ATIMER(check);

  t1 = wctime();
  TC_START_ATIMER(check);
  busy_wait(niter);
  TC_STOP_ATIMER(check);
  t2 = wctime();

  printf("Measured time=%fs, Error=%f %% (%3f)\n", t2-t1, ((t2-t1)-work_time)/work_time*100.0, TC_READ_ATIMER_MSEC(check));
  
  return 0;
}
#endif

