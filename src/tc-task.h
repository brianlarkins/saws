#ifndef __TC_TASK_H__
#define __TC_TASK_H__

#define GTC_CB_NOEXECUTE -1

/** TASK CLASS: Task classes are integers that can be used to look up the
 * corresponding task class description.  They must be registered in the same
 * order on each process.
 **/
typedef int task_class_t;


/** TASK DESCRIPTOR:  Represents an actual task, contains a header which is
 * used by Scioto and an opaque body which contains user supplied task
 * arguments.
 **/
struct task_s {
  task_class_t  task_class;  // Callback handle to evaluate this task
  int           created_by;  // Process ID of this task's creator
  int           affinity;    // Affinity of this task
  int           priority;    // Priority of this task
  char          body[0];     // Opaque payload, everything beyond here is user defined
};
typedef struct task_s task_t;


/** TASK CLASS DESCRIPTION: Task class description.  This contains all of the
 * information about a task class, including the function pointer used to
 * execute the task.
 **/  
struct task_class_desc_s {
  int  body_size;
  void (*cb_execute)(gtc_t gtc, task_t *descriptor); // execution callback
  void *pool; // Allocation pool to avoid alloc cost
};
typedef struct task_class_desc_s task_class_desc_t;


task_class_t gtc_task_class_register(int body_size, void (*cb_execute)(gtc_t gtc, task_t *descriptor));

task_t *gtc_task_alloc(int body_size);
task_t *gtc_task_create(task_class_t tclass);
void    gtc_task_destroy(task_t *task);
void    gtc_task_reuse(task_t *task);

#define gtc_task_set_priority(_TASK, _PRIORITY) (_TASK)->priority = (_PRIORITY)
#define gtc_task_set_affinity(_TASK, _AFFINITY) (_TASK)->affinity = (_AFFINITY)

void          gtc_task_set_class(task_t *task, task_class_t tclass);
task_class_t  gtc_task_get_class(task_t *task);
int           gtc_task_class_largest_body_size(void);

task_class_desc_t *gtc_task_class_lookup(task_class_t tclass);
#define            gtc_task_body_size(TSK) gtc_task_class_lookup((TSK)->task_class)->body_size

void    gtc_task_execute(gtc_t gtc, task_t *task);
#define gtc_task_body(TSK) (&((TSK)->body))

#endif /* __TC_TASK_H__ */
