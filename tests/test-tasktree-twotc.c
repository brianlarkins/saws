#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <tc.h>

#define NCHILDREN   2
#define MAXDEPTH   14
#define SLEEP_TIME 10
#define VERBOSE    1

static int mythread, nthreads;

typedef struct {
  int        which_tc;
  int        level;
  int        index;
  int        counter_key1;
  int        counter_key2;
  gtc_t gtc1;
  gtc_t gtc2;
} treetask_t;


/**
 * Add a task to the proper task collection and set it up to execute in
 * the context of that collection.
 **/
static void add_task(task_t *task) {
  treetask_t *tt = (treetask_t*) gtc_task_body(task);

  gtc_add((tt->which_tc == 0) ? tt->gtc1 : tt->gtc2, task, mythread);
}


/**
 * Task's execution callback.  Generates subtasks, alternating between TCs
 * for each new task.
 **/
void task_fcn(gtc_t gtc, task_t *descriptor) {
  int         i, level, index;
  treetask_t *tt = (treetask_t *) gtc_task_body(descriptor);
  int        *ctr= (int *) gtc_clo_lookup(gtc, tt->which_tc == 0 ? tt->counter_key1 : tt->counter_key2); 

  if (VERBOSE) printf("  Task (%2d, %3d) processed by worker %d\n", tt->level, tt->index, mythread);

  level = tt->level;
  index = tt->index;

  for (i = 0; i < NCHILDREN && level < MAXDEPTH; i++) {
    tt->level    = level + 1;
    tt->index    = 2*index + i;
    tt->which_tc = i%2;
    if (VERBOSE) printf("  Task (%2d, %3d) created by worker %d\n", tt->level, tt->index, mythread);
    add_task(descriptor);
  }

  usleep(SLEEP_TIME);

  ++(*ctr);
}


int main(int argc, char **argv) {
  int sum, expected, this_iter, counter;
  task_class_t task_class;
  int        counter_key1;
  int        counter_key2;
  gtc_t gtc1;
  gtc_t gtc2;

  gtc1 = gtc_create(sizeof(treetask_t), 10, 10000, NULL, GtcQueueSDC);
  gtc2 = gtc_create(sizeof(treetask_t), 10, 10000, NULL, GtcQueueSDC);

  mythread = _c->rank;
  nthreads = _c->size;

  task_class   = gtc_task_class_register(sizeof(treetask_t), task_fcn); // Register the task class in GTC1

  counter_key1 = gtc_clo_associate(gtc1, &counter); // Create a CLO association for the counter in GTC1
  counter_key2 = gtc_clo_associate(gtc2, &counter); // Create a CLO association for the counter in GTC2

  // Create the root task and place it on process 0 in GTC1
  if (mythread == 0) {
    task_t   *task = gtc_task_create(task_class);
    treetask_t *tt = (treetask_t *) gtc_task_body(task);

    printf("Starting multiple task collection tree test with %d threads\n", nthreads);
    printf("Thread 0: Putting root task in my queue.\n");

    tt->which_tc     = 0;
    tt->level        = 0;
    tt->index        = 0;
    tt->counter_key1 = counter_key1;
    tt->counter_key2 = counter_key2;
    tt->gtc1         = gtc1;
    tt->gtc2         = gtc2;

    add_task(task);

    gtc_task_destroy(task); // Free the task.  Copy-in/out semantics

    printf("Tree test starting...\n");
  }

  // Process GTC1 and GTC2 iteratively until there is no more work
  for (this_iter = 1, sum = 0; this_iter > 0; ) {
    counter = 0;

    if (mythread == 0) printf(" + processing gtc 1\n");
    gtc_process(gtc1);
    gtc_reset(gtc1);
    if (mythread == 0) printf(" + processing gtc 2\n");
    gtc_process(gtc2);
    gtc_reset(gtc2);

    gtc_allreduce(&counter, &this_iter, GtcReduceOpSum, IntType, 1);
    sum += this_iter;
    
    if (mythread == 0) printf(" - this round = %4d, total = %4d\n", this_iter, sum);
  }

  expected = (1 << (MAXDEPTH+1)) - 1; //  == 2^(MAXDEPTH + 1) - 1

  if (mythread == 0)
    printf("Total tasks processed = %d, expected = %d: %s\n", sum, expected,
        (sum == expected) ? "SUCCESS" : "FAILURE");

  gtc_destroy(gtc1);
  gtc_destroy(gtc2);

  return 0;
}
