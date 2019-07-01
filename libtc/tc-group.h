#ifndef _TC_GROUP_H_
#define _TC_GROUP_H_

// #include <ga.h>
#include <tc.h>


typedef struct {
  int *procs; // Mapping table
  int nodeid; // ID of the local node (-1 if not in the group)
  int nnodes; // Number of nodes in the group
} gtc_pgroup_t;


/* EXEC_GROUP - GTC execution group management routines.
 *
 * Every task collection will have execution, default, and steal groups
 * regardless of whether the user has enabled groups.  The execution and
 * default group can be provided by the user; the steal group is a group of all
 * processes that have work queues.  When no groups are used, the execution
 * group is a "self" group and the steal group contains all processes.  In this
 * case, ranks in the steal group and default group will be identical and can
 * be used interchangeably.
 *
 * "exec" group routines are intended to be the public interface and "steal"
 * group routines are the internal interface.
 */

/* Look up GA group ids */
int gtc_group_get_exec(gtc_t gtc);
int gtc_group_get_default(gtc_t gtc);
gtc_pgroup_t *gtc_group_get_steal(gtc_t gtc);

/* Public execution group interface */
int gtc_group_exec_is_global_master(gtc_t gtc);
int gtc_group_exec_ismaster(gtc_t gtc);
int gtc_group_exec_master_rank(gtc_t gtc);
int gtc_group_exec_groupid(gtc_t gtc);
int gtc_group_exec_ngroups(gtc_t gtc);

/* Private steal group interface */
int gtc_group_steal_nodeid(gtc_t gtc);
int gtc_group_steal_nnodes(gtc_t gtc);
int gtc_group_steal_ismember(gtc_t gtc);

/* Assign/cleanup groups */
void gtc_group_set(gtc_t gtc, int default_group, int exec_group);
void gtc_group_set_nogroups(gtc_t gtc);
void gtc_group_cleanup(gtc_t gtc);


/* PGROUP - GTC process group functions.
 *
 * Create a processor group that maps between group ranks and world ranks.
 * This is used only to establish a mapping, group ranks should be mapped to
 * work ranks in order to perform communication. 
 */

#define gtc_pgroup_nodeid(_PGRP) (_PGRP)->nodeid
#define gtc_pgroup_nnodes(_PGRP) (_PGRP)->nnodes
#define gtc_pgroup_rank_to_world(_PGRP, _GRPID) (_PGRP)->procs[(_GRPID)]
#define gtc_pgroup_ismember(_PGRP) (gtc_pgroup_nodeid(_PGRP) >= 0)

gtc_pgroup_t *gtc_pgroup_create(gtc_t gtc, int is_member);
void          gtc_pgroup_destroy(gtc_pgroup_t *group);

#endif

