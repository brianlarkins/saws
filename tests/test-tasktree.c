/** Tasktree.c -- Dynamically generate and execute a tree of tasks
 *
 * Author: James Dinan <dinan.10@osu.edu>
 * Date  : August, 2008
 * 
 * An example Scioto parallel program that executes a tree of tasks.  Execution
 * begins with a single root task, this task spawns NCHILDREN tasks, each child
 * spawns NCHILDREN tasks, and so on until the MAXDEPTH is reached.  We keep
 * track of the total number of tasks that were executed in a replicated local
 * object, counter.  Upon completion we perform an MPI reduction on the counter
 * to check that the correct number of tasks were executed.
 *
 */

#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <tc.h>

#define NCHILDREN  2
#define MAXDEPTH   14
#define SLEEP_TIME 100
#define VERBOSE    0

#ifdef PUSHING
#define QSIZE      100000
#else
#define QSIZE      20
#endif

static int mythread, nthreads;

typedef struct {
  int parent_id;
  int level;
  int index;
  int counter_key;
} treetask_t;


/**
 * Wrapper to create tasks and enqueue them in the task collection
 *
 * @param gtc         The task collection to enqueue the task into
 * @param tclass      Portable reference to the task class
 * @param level       Level of this task in the tree
 * @param index       Index of this task in the tree
 * @param counter_key The CLO key that is used to look up the counter
**/
static void create_task(gtc_t gtc, task_class_t tclass, int level, int index, int counter_key) {
  task_t     *task = gtc_task_create(tclass);        
  treetask_t *tt   = (treetask_t*) gtc_task_body(task);

  tt->parent_id   = mythread;
  tt->level       = level;
  tt->index       = index;
  tt->counter_key = counter_key;

#ifdef PUSHING
  gtc_add(gtc, task, index % nthreads);
#else
  gtc_add(gtc, task, mythread);
#endif

  gtc_task_destroy(task);
}


/**
 * This function implements the task and is called when the task is executed.
 *
 * @param gtc        The task collection that is being executed on
 * @param descriptor The task's descriptor (contains user supplied arguments)
**/
void task_fcn(gtc_t gtc, task_t *descriptor) {
  int         i;
  treetask_t *tt = (treetask_t *) gtc_task_body(descriptor);
  int        *ctr= (int *) gtc_clo_lookup(gtc, tt->counter_key); 
        
  for (i = 0; i < NCHILDREN && tt->level < MAXDEPTH; i++)
    create_task(gtc, descriptor->task_class, tt->level + 1, 2*tt->index + i, tt->counter_key);

  *ctr += 1;
  usleep(SLEEP_TIME);

  if (VERBOSE) printf("  Task (%2d, %2d, %3d) processed by worker %d\n", 
    tt->parent_id, tt->level, tt->index, mythread);
}


int main(int argc, char **argv) {
  int           counter = 0;   // Count the number of tasks executed
  int           sum, expected; // Used to check the final result
  gtc_t         gtc;           // Portable reference to a task collection
  int           counter_key;   // A portable reference to replicated local copies of counter
  task_class_t  task_class;    // A task class defines a type of task
  gtc_qtype_t   qtype = GtcQueueSDC; // default is to use baseline queue
  int arg;

  while ((arg  = getopt(argc, argv, "BHN")) != -1) {
    switch (arg) {
      case 'B':
        qtype = GtcQueueSDC;
        break;
      case 'H':
        qtype = GtcQueuePortalsHalf;
        break;
      case 'N':
        qtype = GtcQueuePortalsN;
        break;
    }
  }

  gtc = gtc_create(sizeof(treetask_t), 10, QSIZE, NULL, qtype);

  mythread = _c->rank;
  nthreads = _c->size;

  counter_key = gtc_clo_associate(gtc, &counter);       // Collectively register counter
  task_class  = gtc_task_class_register(sizeof(treetask_t), task_fcn); // Collectively create a task class

  // Add the root task to the task collection
  if (mythread == 0) {
    gtc_print_config(gtc);
    printf("Starting task collection tree test with %d threads\n", nthreads);
    printf("Thread 0: Putting root task in my queue.\n");
                
    create_task(gtc, task_class, 0, 0, counter_key);

    printf("Tree test starting...\n");
  }

  // Process the task collection
  gtc_process(gtc);

  // Check if the correct number of tasks were processed
  gtc_reduce(&counter, &sum, GtcReduceOpSum, IntType, 1);
  expected = (1 << (MAXDEPTH+1)) - 1; // == 2^(MAXDEPTH + 1) - 1

  if (mythread == 0) {
    printf("Total tasks processed = %d, expected = %d: %s\n", sum, expected,
      (sum == expected) ? "SUCCESS" : "FAILURE");
    printf("Total task time = %f sec, ideal walltime = %f sec\n", SLEEP_TIME*(float)sum/1e6,
      SLEEP_TIME*(float)sum/1e6/nthreads);
  }

  gtc_barrier();

  gtc_print_stats(gtc);
  gtc_destroy(gtc);

  return 0;
}
