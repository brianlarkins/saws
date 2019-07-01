#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

#include <tc.h>

#define NUM_TASKS  1000
#define SLEEP_TIME 100
#define VERBOSE    0

typedef struct {
  int parent_id;
  int task_num;
} mytask_t;

static int mythread, nthreads;

static void create_task(gtc_t gtc, task_class_t tclass, int target, int my_id, int task_num);
void task_fcn(gtc_t gtc, task_t *task);

/**
 * Sample task to be placed into the work pool.
 **/
void task_fcn(gtc_t gtc, task_t *task) {
  mytask_t *t = (mytask_t*) gtc_task_body(task);

  usleep(t->task_num * SLEEP_TIME);
  if (VERBOSE) printf("  Task (%2d, %3d) processed by worker %d\n", t->parent_id, t->task_num, mythread);
}


/**
 * Put an instance of task_fcn() into the work pool.
 *
 * @param tc        Where to enqueue the task.
 * @param tclass    Task's class
 * @param my_id     ID of the thread that generated this task.
 * @param task_num  Sequence number for output by task_fcn()
 **/
static void create_task(gtc_t gtc, task_class_t tclass, int target, int my_id, int task_num) {
  task_t   *task = gtc_task_create(tclass);
  mytask_t *t    = (mytask_t*) gtc_task_body(task);

  t->parent_id = my_id;
  t->task_num  = task_num;

  gtc_add(gtc, task, target);
  gtc_task_destroy(task);
}


int main(int argc, char **argv)
{
  int i, sum;
  task_class_t task_class;
  gtc_t gtc;

  // zomg. ordering dependencies.
  gtc_init();

  mythread = _c->rank;
  nthreads = _c->size;

  gtc = gtc_create(sizeof(mytask_t), 10, NUM_TASKS/nthreads+1, NULL, GtcQueueSDC);

  task_class = gtc_task_class_register(sizeof(mytask_t), task_fcn);

  /** POPULATE THE TASK LIST **/
#ifdef PUSHING
  if (mythread == 0) {
    printf("Starting round robin task collection test with %d threads\n", nthreads);
    printf("Thread 0: Populating TCs with workload\n");

    for (i = 0; i < NUM_TASKS; i++)
      create_task(gtc, task_class, i % nthreads, mythread, i);

    printf("Round robin scheduled test starting ...\n");
  }
#else
  if (mythread == 0) printf("Starting round robin task collection test with %d threads\n", nthreads);

  for (i = 0; i < NUM_TASKS; i++)
    if (i % nthreads == mythread)
      create_task(gtc, task_class, mythread, mythread, i);

  if (mythread == 0) printf("Round robin scheduled test starting ...\n");
#endif

  gtc_barrier();
  gtc_process(gtc);

  // Check if the correct number of tasks were processed
  tc_t *tc = gtc_lookup(gtc);
  gtc_reduce(&tc->tasks_completed, &sum, GtcReduceOpSum, IntType, 1);

  if (mythread == 0)
    printf("Total tasks processed = %d, expected = %d: %s\n", sum, NUM_TASKS,
        (sum == NUM_TASKS) ? "SUCCESS" : "FAILURE");

  gtc_barrier();

  gtc_print_stats(gtc);
  gtc_destroy(gtc);

  return 0;
}
