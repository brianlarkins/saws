#ifndef __TC_H__
#define __TC_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef int gtc_t;

#include <tc-task.h>
// #include <tc-group.h>

#define AUTO_BODY_SIZE -1

enum victim_select_e { VICTIM_RANDOM, VICTIM_ROUND_ROBIN };
enum steal_method_e  { STEAL_HALF, STEAL_ALL, STEAL_CHUNK };

typedef struct {
  int stealing_enabled;          /* Is stealing enabled?  If not, the load balance is static with pushing */
  int victim_selection;          /* One of victim_select_e */
  int steal_method;              /* One of steal_method_e  */
  int steals_can_abort;          /* Allow aborting steals  */
  int max_steal_retries;         /* Max number of retries before we abort.  Set low to limit contention. -1 means infinity */
  int max_steal_attempts_local;  /* Max number of lock attempts before we "retry" a local victim. */
  int max_steal_attempts_remote; /* Max number of lock attempts before we "retry" a remote victim. */
  int chunk_size;                /* Size of a steal when using STEAL_CHUNK */
  int local_search_factor;       /* Percent of steal attempts (0-100) that should target intra-node victims */
} gtc_ldbal_cfg_t;


/** GTC Construction/Destruction **/
void    gtc_ldbal_cfg_init(gtc_ldbal_cfg_t *cfg);
void    gtc_ldbal_cfg_set(gtc_t gtc, gtc_ldbal_cfg_t *cfg);
void    gtc_ldbal_cfg_get(gtc_t gtc, gtc_ldbal_cfg_t *cfg);
gtc_t   gtc_create(int max_body_size, int shrb_size, gtc_ldbal_cfg_t *ldbal_cfg);
gtc_t   gtc_create_grouped(int max_body_size, int shrb_size, gtc_ldbal_cfg_t *ldbal_cfg, int exec_group);
void    gtc_destroy(gtc_t gtc);
void    gtc_reset(gtc_t gtc);
void    gtc_queue_reset(gtc_t gtc);
void    gtc_progress(gtc_t gtc);

/** GTC Configuration **/
void    gtc_enable_stealing(gtc_t gtc);
void    gtc_disable_stealing(gtc_t gtc);
void    gtc_print_config(gtc_t gtc);
char   *gtc_queue_name(void);
void    gtc_set_external_work_avail(gtc_t tc, int flag);

/** Adding Tasks **/
int     gtc_add(gtc_t gtc, task_t *task, int master);
task_t *gtc_task_inplace_create_and_add(gtc_t gtc, task_class_t tclass);
void    gtc_task_inplace_create_and_add_finish(gtc_t gtc, task_t *task);

/** Processing the GTC **/
void    gtc_process(gtc_t gtc);
task_t *gtc_get          (gtc_t gtc, int priority);
int     gtc_get_buf      (gtc_t gtc, int priority, task_t *buf);
task_t *gtc_get_local    (gtc_t gtc, int priority);
int     gtc_get_local_buf(gtc_t gtc, int priority, task_t *buf);

/** Common Local Objects **/
int     gtc_clo_associate(gtc_t gtc, void *ptr);
void   *gtc_clo_lookup(gtc_t gtc, int id);
void    gtc_clo_assign(gtc_t gtc, int id, void *ptr);
void    gtc_clo_reset(gtc_t gtc);

/** GTC Information **/
int     gtc_tasks_avail(gtc_t gtc);
void    gtc_print_stats(gtc_t gtc);

/** Statistics **/
double  gtc_wctime(void);
unsigned long gtc_stats_tasks_completed(gtc_t gtc);
unsigned long gtc_stats_tasks_created(gtc_t gtc);


#ifdef __cplusplus
}
#endif


#endif /*__TC_H__*/
