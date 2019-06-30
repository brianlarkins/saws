#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

#include <tc.h>

#define NUM_TASKS 50

typedef struct {
  int parent_id;
  int task_num;
} mytask_t;

static task_class_t task_class;
static int mythread, nthreads;
static long sleep_time = 0;

void create_task(gtc_t gtc, task_class_t tclass, int my_id, int task_num);
void task_fcn(gtc_t gtc, task_t *task);

/**
 * Sample task to be placed into the work pool.  May generated subtasks.
 **/
void task_fcn(gtc_t gtc, task_t *task) {
  int timeout;
  mytask_t *t = (mytask_t*) gtc_task_body(task);

  if (rand() < RAND_MAX/2)
    // 50% Chance of spawning a new task
    create_task(gtc, task_class, mythread, t->task_num+1);

  timeout = rand() % 1000000;
  usleep(timeout);
  sleep_time += timeout;
  printf("  Task (%2d, %3d) processed by worker %d\n", t->parent_id, t->task_num, mythread);
}

/**
 * Put an instance of task_fcn() into the work pool.
 *
 * @param tc        Where to enqueue the task.
 * @param tclass    Task's class
 * @param my_id     ID of the thread that generated this task.
 * @param task_num  Sequence number for output by task_fcn()
 **/
void create_task(gtc_t gtc, task_class_t tclass, int my_id, int task_num) {
  task_t   *task = gtc_task_create(tclass);
  mytask_t *t    = (mytask_t*) gtc_task_body(task);

  t->parent_id = my_id;
  t->task_num  = task_num;

  gtc_add(gtc, task, mythread);
  gtc_task_destroy(task);
}


int main(int argc, char **argv)
{
  int   i;
  gtc_t gtc;
  long  ideal_time = 0;

  printf("starting run\n"); fflush(stdout);

  gtc = gtc_create(sizeof(mytask_t), 3, NUM_TASKS, NULL, GtcQueueSDC);
  gtc_barrier();

  printf("created\n"); fflush(stdout);
  mythread = gtc_mythread();
  nthreads = gtc_nthreads();

  for (int i=0; i<nthreads; i++) {
    if (mythread == i) {
      printf("hello from %d\n", i);
    }
    gtc_barrier();
  }
  gtc_barrier();

  task_class = gtc_task_class_register(sizeof(mytask_t), task_fcn);

  // Per-process repeatable randomness
  srand(mythread);

  /** POPULATE THE TASK LIST **/
  if (mythread == 0) {
    printf("Starting task collection test with %d threads\n", nthreads);
    gtc_print_config(gtc);
    printf("Thread 0: Populating my TC with initial workload\n");

    for (i = 0; i < NUM_TASKS; i++)
      create_task(gtc, task_class, mythread, i);

    printf("Thread 0: done.\n");
  }

  gtc_process(gtc);

  gtc_print_stats(gtc);

  gtc_barrier();

  // Find the ideal execution time
  //MPI_Reduce(&sleep_time, &ideal_time, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
  gtc_reduce(&sleep_time, &ideal_time, GtcReduceOpSum, LongType, 1);
  if (mythread == 0)
    printf("Total sleep time = %f sec, Ideal = %f sec (compare with process time above)\n",
        ideal_time/1e6, ideal_time/1e6/nthreads);

  gtc_destroy(gtc);


  return 0;
}
