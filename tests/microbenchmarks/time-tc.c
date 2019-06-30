#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <tc.h>

#define TASK_SIZE  1024
#define NITER      100000


void task_fcn(gtc_t gtc, task_t *task) {
  return;
}


int main(int argc, char **argv)
{
  int i;
  //int nsteals;
  double t_add_l   = 0.0;
  //double t_add_u   = 0.0;
  double t_drain_l = 0.0;
  //double t_drain_u = 0.0;
  //double t_add_r   = 0.0;
  //double t_steal   = 0.0;
  double avg_t_add, avg_t_drain;
  task_t *task;
  //void   *tasks;
  tc_timer_t addtimer, draintimer;

  task_class_t task_class;
  gtc_t  gtc;
  int    mythread, nthreads;
  int chunk_size = 1;

  gtc = gtc_create(TASK_SIZE, chunk_size, NITER, NULL, GtcQueueSDC);
  TC_INIT_ATIMER(addtimer);
  TC_INIT_ATIMER(draintimer);

  mythread = _c->rank;
  nthreads = _c->size;

  gtc_disable_stealing(gtc);

  task_class  = gtc_task_class_register(TASK_SIZE, task_fcn);
  task        = gtc_task_create(task_class);
  //tasks       = gtc_malloc(CHUNK_SIZE * (sizeof(task_t) + TASK_SIZE));

  //assert(tasks != NULL);

  if (mythread == 0) printf("Starting task collection timing with %d threads\n", nthreads);

  /**** LOCAL TASK INSERTION ****/

  gtc_barrier();

  if (mythread == 0)
    printf("Timing: Local task insertion (%d tasks)\n", NITER);

  //t_add_l = gtc_wctime();
  TC_START_ATIMER(addtimer);
  for (i = 0; i < NITER; i++) {
    gtc_add(gtc, task, mythread);
  }
  //t_add_l = gtc_wctime() - t_add_l;
  TC_STOP_ATIMER(addtimer);
  t_add_l = TC_READ_ATIMER_SEC(addtimer);

  printf("  %d: %f sec (%f tasks/sec, %f usec/task)\n", mythread, TC_READ_ATIMER_SEC(addtimer), 
                                                       NITER/TC_READ_ATIMER_SEC(addtimer), 
                                                       TC_READ_ATIMER_USEC(addtimer)/NITER);
  //MPI_Reduce(&t_add_l, &avg_t_add, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  gtc_reduce(&t_add_l, &avg_t_add, GtcReduceOpSum, DoubleType, 1);
  gtc_barrier();

  /**** LOCAL TASK DRAIN ****/

  if (mythread == 0)
    printf("Timing: Local task pool throughput\n");

  //t_drain_l = gtc_get_wtime();
  TC_START_ATIMER(draintimer);
  gtc_process(gtc);
  //for (i=0; gtc_get_buf(tc, 0, task); i++);
  //t_drain_l = gtc_get_wtime() - t_drain_l;
  TC_STOP_ATIMER(draintimer);
  t_drain_l = TC_READ_ATIMER_SEC(draintimer);

  printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", mythread, TC_READ_ATIMER_SEC(draintimer),
                                                       NITER/TC_READ_ATIMER_SEC(draintimer), 
                                                       TC_READ_ATIMER_SEC(draintimer)/NITER);
  //MPI_Reduce(&t_drain_l, &avg_t_drain, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  gtc_reduce(&t_drain_l, &avg_t_drain, GtcReduceOpSum, DoubleType, 1);
  gtc_barrier();
  //gtc_print_stats(tc);
  if (mythread == 0) {
    printf("AVG ADD  : %f sec (%f tasks/sec, %e sec/task)\n", avg_t_add/nthreads,
        NITER/(avg_t_add/nthreads), (avg_t_add/nthreads)/NITER);
    printf("AVG DRAIN: %f sec (%f tasks/sec, %e sec/task)\n", avg_t_drain/nthreads,
        NITER/(avg_t_drain/nthreads), (avg_t_drain/nthreads)/NITER);
  }
  gtc_barrier();

#if 0
  gtc_reset(gtc);
  /**** REMOTE TASK INSERTION ****/

  gtc_barrier();

  if (mythread == 0)
    printf("\nTiming: Remote task insertion (%d tasks)\n", NITER);

  t_add_r = gtc_wctime();
  for (i = 0; i < NITER; i++) {
    gtc_add(gtc, task, (mythread+1)%nthreads);
  }
  t_add_r = gtc_wctime() - t_add_r;

  printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", mythread, t_add_r, NITER/t_add_r, t_add_r/NITER);
  MPI_Reduce(&t_add_r, &avg_t_add, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

  gtc_progress(gtc);
#ifndef TC_LOCKED
  split_shrb_release_everything(tc->shared_rb);
#endif
  gtc_barrier();

  /**** STEALING PERFORMANCE ****/

  if (mythread == 0)
    printf("Timing: Steal Performace, chunk size = %d\n", CHUNK_SIZE);

  assert(nthreads > 1); // Limitation from the split shrb - can't locally pop_n_tail

  t_steal = gtc_wctime();
  for (nsteals = 0; split_shrb_pop_n_tail(tc->shared_rb, (mythread+1)%nthreads, CHUNK_SIZE, tasks) > 0; nsteals++);
  t_steal = gtc_wctime() - t_steal;

  printf("  %d: %f sec (%f steals/sec, %e sec/steal)\n", mythread, t_steal, nsteals/t_steal, t_steal/nsteals);
  MPI_Reduce(&t_steal, &avg_t_drain, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  gtc_barrier();
  //gtc_print_stats(tc);
  if (mythread == 0) {
    printf("AVG ADD_R: %f sec (%f tasks/sec, %e sec/task)\n", avg_t_add/nthreads,
        NITER/(avg_t_add/nthreads), (avg_t_add/nthreads)/NITER);
    printf("AVG STEAL: %f sec (%f steals/sec, %e sec/steal, %d total steals)\n", avg_t_drain/nthreads,
        nsteals/(avg_t_drain/nthreads), (avg_t_drain/nthreads)/nsteals, nsteals);
  }
  gtc_barrier();

  //#if 0
  /**** UNLOCKED TASK INSERTION ****/
  gtc_reset(tc);
  gtc_begin_single(tc, tc->mythread);
  gtc_barrier();

  if (tc->mythread == 0)
    printf("\nTiming: Unlocked task insertion (%d tasks)\n", NITER);

  t_add_u = gtc_wctime();
  for (i = 0; i < NITER; i++) {
    gtc_add(tc, task, tc->mythread);
  }
  t_add_u = gtc_wctime() - t_add_u;

  printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", tc->mythread, t_add_u, NITER/t_add_u, t_add_u/NITER);
  gtc_barrier();

  /**** UNLOCKED TASK DRAIN ****/

  if (tc->mythread == 0)
    printf("Timing: Unlocked task pool throughput\n");

  t_drain_u = gtc_wctime();
  gtc_process(tc);
  t_drain_u = gtc_wctime() - t_drain_u;

  printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", tc->mythread, t_drain_u, NITER/t_drain_u, t_drain_u/NITER);
  gtc_barrier();
  gtc_print_stats(tc);
  gtc_barrier();

  /**** DONE ****/
#endif

  gtc_task_destroy(task);
  gtc_destroy(gtc);

  return 0;
}
