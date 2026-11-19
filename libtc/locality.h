/********************************************************/
/*                                                      */
/*  locality.h - node membership for locality-aware     */
/*               work stealing (LAWS)                   */
/*                                                      */
/********************************************************/

#pragma once

#include <shmem.h>

/*
 * Node membership is discovered once in gtc_init() from the PEs' host names, so
 * it is correct for any rank placement and any OpenSHMEM build.  When
 * SHMEM_TEAM_SHARED spans more than one PE (e.g. SOS with XPMEM) it must agree
 * with the host-name grouping.
 */
typedef struct {
  int           node;             // my node id (nodes numbered by lowest world rank)
  int           nnodes;           // number of nodes
  int           node_rank;        // my index within node_pes
  int           node_size;        // PEs on my node (including me)
  int          *node_pes;         // world ranks of the PEs on my node, ascending
  int          *node_of;          // world rank -> node id, for all PEs
  int          *node_rank_of;     // world rank -> index within its node (bit position in node bitmaps)
  int          *node_start;       // node n's members are node_members[node_start[n] .. node_start[n+1]-1]
  int          *node_members;     // world ranks grouped by node, ascending within each node
  int           far_count;        // PEs not on my node
  int           contiguous;       // every node holds an equal, contiguous block of ranks
  int           shared_team_size; // size of SHMEM_TEAM_SHARED (1 = no on-node shared memory)
  int           ptr_ok;           // shmem_ptr reaches every node-mate
  shmem_team_t  node_team;        // my node's team, or SHMEM_TEAM_INVALID if not contiguous
} gtc_locality_t;

void   gtc_locality_init(void);           // collective over SHMEM_TEAM_WORLD
void   gtc_locality_fini(void);
void   gtc_locality_print(void);          // one summary line (call from one PE)

int    gtc_random_near_peer(void);        // random node-mate other than me, or -1
int    gtc_random_other_node(void);       // random node id other than mine, or -1
int    gtc_random_far_peer(void);         // random PE on another node, or -1
void **gtc_near_ptr_table(void *symaddr); // shmem_ptr(symaddr, node_pes[i]) for each node-mate (malloc'd)

// is this PE on my node?  (macro so that it can use the global context _c)
#define gtc_is_near(pe) (_c->loc.node_of[(pe)] == _c->loc.node)
// first PE of node n (it holds the node's bitmap), and node n's i-th PE
#define gtc_node_leader(n)    (_c->loc.node_members[_c->loc.node_start[(n)]])
#define gtc_node_member(n, i) (_c->loc.node_members[_c->loc.node_start[(n)] + (i)])
#define gtc_node_size(n)      (_c->loc.node_start[(n)+1] - _c->loc.node_start[(n)])
