#include <iostream>
#include <deque>
using namespace std;

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <tc.h>

#include "uts.h"

#include "UTSIterator.h"

#define UTS_QUEUE_SIZE 100000

static task_class_t task_class;
static int me, nproc;

extern gtc_qtype_t qtype;
int walklen = 1;

void strict_dfs_task_fcn(gtc_t gtc, task_t *parent) {
  UTSIterator *iter  = (UTSIterator*) gtc_task_body(parent);
  task_t      *child = gtc_task_create(task_class);
  
  if (iter->hasNext()) {
    gtc_task_reuse(child); // Reset and reuse local task buffer
    UTSIterator *child_iter = (UTSIterator*) gtc_task_body(child);
    *child_iter = iter->next();

    // If the parent has more children, put it back in
    if (iter->hasNext())
      gtc_add(gtc, parent, me);
    else
      iter->process();

    // Now put in the child
    gtc_add(gtc, child, me);

  } else {
    iter->process();
  }

  gtc_task_destroy(child);
}


void task_fcn(gtc_t gtc, task_t *task) {
  UTSIterator *iter   = (UTSIterator*) gtc_task_body(task);
#ifndef INPLACE
  task_t       *child = gtc_task_create(task_class);
#endif
  
  iter->process();
  
  while (iter->hasNext()) {
#ifdef INPLACE
    task_t *child = gtc_task_inplace_create_and_add(gtc, task_class);
#else
    gtc_task_reuse(child); // Reset and reuse local task buffer
#endif
    UTSIterator *child_iter = (UTSIterator*) gtc_task_body(child);
    iter->next(child_iter);
    //*child_iter = iter->next();
#ifndef INPLACE
    gtc_add(gtc, child, me);
#endif
  }
  
#ifndef INPLACE
  gtc_task_destroy(child);
#endif
}


double ldbal_scioto(UTSIterator iter) {
#ifdef STRICT_DFS
  task_class = gtc_task_class_register(sizeof(UTSIterator), strict_dfs_task_fcn);
#else
  task_class = gtc_task_class_register(sizeof(UTSIterator), task_fcn);
#endif

  setenv("SCIOTO_DISABLE_PERNODE_STATS", "1", 1);
  setenv("GTC_RECLAIM_FREQ", "20", 1);

  // Initialize the Task Collection
  gtc_ldbal_cfg_t cfg;
  gtc_ldbal_cfg_init(&cfg);

  gtc_t         gtc        = gtc_create(sizeof(UTSIterator), 10, UTS_QUEUE_SIZE, &cfg, qtype);
  task_t       *parent     = gtc_task_create(task_class);

  me    = _c->rank;
  nproc = _c->size;

#ifdef INITIAL_BFS

#ifndef INITIAL_SOURCES
#define INITIAL_SOURCES nproc
#endif
#ifndef INITIAL_TASKS
#define INITIAL_TASKS nproc
#endif

  int work_count = 0;
  int work_id    = 0;
  deque<UTSIterator> initialWork(INITIAL_TASKS);

  initialWork.push_back(iter);

  if (me == 0)
     printf("Performing initial BFS to generate %d tasks stored across %d work sources\n\n", INITIAL_TASKS, INITIAL_SOURCES);
  
  while (work_count < INITIAL_TASKS && initialWork.size() > 0) {
    UTSIterator &cur_iter = initialWork.front();
    
    // Expand children into deque
    while (cur_iter.hasNext()) {
      initialWork.push_back(cur_iter.next());
      work_count++;
    }
    
    // Put parent into the task collection
    if (work_id % INITIAL_SOURCES == me) {
        UTSIterator *parent_iter = (UTSIterator*) gtc_task_body(parent);
        *parent_iter = cur_iter;
        gtc_add(gtc, parent, me);
    }

    initialWork.pop_front();
    work_id++;
  }

  // Drain the deque
  while (!initialWork.empty()) {
    UTSIterator &cur_iter = initialWork.front();
    // Put parent into the task collection
    if (work_id % INITIAL_SOURCES == me) {
        UTSIterator *parent_iter = (UTSIterator*) gtc_task_body(parent);
        *parent_iter = cur_iter;
        gtc_add(gtc, parent, me);
    }

    initialWork.pop_front();
    work_id++;
  }

#else /* INITIAL_BFS */

  // Add the given iterator to seed the task pool
  if (me == 0) {
    gtc_task_reuse(parent);
    UTSIterator *parent_iter = (UTSIterator*) gtc_task_body(parent);
    *parent_iter = iter;
    gtc_add(gtc, parent, me);
  }
#endif

  shmem_barrier_all();

  double process_time;
  tc_timer_t ptimer;
  TC_INIT_ATIMER(ptimer);
  TC_START_ATIMER(ptimer);
  gtc_process(gtc);
  TC_STOP_ATIMER(ptimer);
  process_time = TC_READ_ATIMER_SEC(ptimer);
  
  gtc_print_stats(gtc);

  gtc_task_destroy(parent);
  gtc_destroy(gtc);

  return process_time;
}


