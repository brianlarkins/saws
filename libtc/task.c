/*
 * Copyright (C) 2018. See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>

#include "tc.h"

#ifdef _USING_TSC
#define ENABLE_TASK_TIMING
#endif

static int               task_class_count = 0;             // Number of registered callbacks
static task_class_desc_t task_class_reg[GTC_MAX_TASK_CLASSES]; // Registry of task class descriptions

/**
 * Register a task class with Scioto.  This is a collective call.
 *
 * @param cb_execute IN  Function pointer to the function that executes this class of tasks
 * @return               Portable task class ID
 */
task_class_t gtc_task_class_register(int body_size, void (*cb_execute)(gtc_t gtc, task_t *descriptor)) {
  int next = task_class_count;
  assert(next < GTC_MAX_TASK_CLASSES);

  task_class_reg[next].body_size  = body_size;
  task_class_reg[next].cb_execute = cb_execute;
  task_class_reg[next].pool       = NULL;
  ++task_class_count;

  gtc_eprintf(DBGINIT, "  registered task class %d (%p)\n", next, task_class_reg[next].cb_execute);

  return next;
}


/**
 * Look up a task class description.
 *
 * @param tclass  IN  Portable reference to a task class
 * @return            Local pointer to a task class description
 */
task_class_desc_t *gtc_task_class_lookup(task_class_t tclass) {

  assert(tclass < task_class_count && tclass >= 0);

  return &task_class_reg[tclass];
}


/**
 * Return the size of the largest task class
 *
 * @return size in bytes
 */
int gtc_task_class_largest_body_size(void) {
  int i;
  int max_size = 0;

  // Sanity: Make sure the user has defined some task classes
  assert(task_class_count > 0);

  for (i = 0; i < task_class_count; i++) {
    if (task_class_reg[i].body_size > max_size)
      max_size = task_class_reg[i].body_size;
  }

  return max_size;
}


/**
 * Create a new task object.
 */
task_t *gtc_task_alloc(int body_size) {
  task_t *task;

  task = malloc(sizeof(task_t) + body_size);
  assert(task != NULL);

  return task;
}


/**
  * Create a task and set the execute callback.  This is a wrapper for
  * gtc_task_create, provided for convenience.
  */
task_t *gtc_task_create(task_class_t tclass) {
  task_class_desc_t *tdesc = gtc_task_class_lookup(tclass);
  task_t            *task;

  // Check the allocation pool for a task of this class, otherwise alloc a new one.
  if (tdesc->pool != NULL) {
    task = tdesc->pool;
    tdesc->pool = NULL;
  } else {
    task = gtc_task_alloc(tdesc->body_size);
  }

  task->affinity = 0; // Default values for header fields
  task->priority = 0;

  gtc_task_set_class(task, tclass);

  return task;
}


/**
 * Destroy a task.  If there is room in the allocation pool, store the buffer.
 * Otherwise, free it.
 */
void gtc_task_destroy(task_t *task) {
  task_class_desc_t *tdesc = gtc_task_class_lookup(task->task_class);

  if (tdesc->pool == NULL)
    tdesc->pool = task;
  else
    free(task);
}


/**
 * Reuse a task descriptor.  This clears any stats and resets the header, it
 * does not affect any leftover data in the task's body.  It is ok to call reuse
 * on a task descriptor that hasn't been used yet.
 */
void gtc_task_reuse(task_t *task) {
  // This is presently a no-op
  // Reset affinity and priority?
  return;
}


/**
  * Set a task's execution callback handle.
  */
void gtc_task_set_class(task_t *task, task_class_t tclass) {
  task->task_class = tclass;
}


/**
  * Get a task's execution callback handle.
  */
task_class_t gtc_task_get_class(task_t *task) {
  return task->task_class;
}


/**
 * Execute the given task in the context of the given tc
 *
 * @param tc   pointer to task collection
 * @param task task to execute
 */
void gtc_task_execute(gtc_t gtc, task_t *task) {
  tc_t *tc = gtc_lookup(gtc);

  gtc_lprintf(DBGPROCESS, "  processing task of type %d (%p)\n",
        task->task_class, task_class_reg[task->task_class].cb_execute);
  assert(task->task_class < task_class_count); // Ensure this is a valid callback handle

  // Execute the task's callback on this tc and the task descriptor
  task_class_reg[task->task_class].cb_execute(gtc, task);

  tc->tasks_completed++;
  gtc_lprintf(DBGPROCESS, "  task completed\n");
}
