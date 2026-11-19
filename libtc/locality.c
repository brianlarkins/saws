/********************************************************/
/*                                                      */
/*  locality.c - node membership for locality-aware     */
/*               work stealing (LAWS)                   */
/*                                                      */
/********************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tc.h"


/* 64-bit FNV-1a hash of the host name: node identity for every PE. */
static uint64_t host_hash(void) {
  char     host[256];
  uint64_t h = 1469598103934665603ULL;

  if (gethostname(host, sizeof(host)) != 0)
    strcpy(host, "unknown");
  host[sizeof(host)-1] = '\0';
  for (char *p = host; *p; p++) {
    h ^= (uint8_t)*p;
    h *= 1099511628211ULL;
  }
  return h;
}



// GTC_DEBUG_LOCALITY=1: every PE reports each start-up stage on stderr
static int dbg_loc = 0;
#define LOC_STAGE(s) do { if (dbg_loc) fprintf(stderr, "locality rank %d: %s\n", _c->rank, s); } while (0)

/**
 * gtc_locality_init - discover which PEs share my node.  Collective.
 */
void gtc_locality_init(void) {
  gtc_locality_t *L = &_c->loc;
  int       me = _c->rank, npes = _c->size;
  uint64_t *hashes, *mine;
  char     *dbg = getenv("GTC_DEBUG_LOCALITY");

  dbg_loc = (dbg && atoi(dbg) != 0);
  LOC_STAGE("start");

  memset(L, 0, sizeof(*L));
  L->node_team = SHMEM_TEAM_INVALID;

  // gather every PE's host-name hash
  hashes = shmem_malloc(npes * sizeof(uint64_t));
  mine   = shmem_malloc(sizeof(uint64_t));
  assert(hashes && mine);
  *mine = host_hash();
  LOC_STAGE("malloc done");
  shmem_barrier_all();
  LOC_STAGE("barrier done");
  shmem_uint64_fcollect(SHMEM_TEAM_WORLD, hashes, mine, 1);
  LOC_STAGE("fcollect done");

  // number the nodes by their lowest world rank
  L->node_of = malloc(npes * sizeof(int));
  assert(L->node_of);
  L->nnodes = 0;
  for (int p = 0; p < npes; p++) {
    L->node_of[p] = -1;
    for (int q = 0; q < p; q++) {
      if (hashes[q] == hashes[p]) {
        L->node_of[p] = L->node_of[q];
        break;
      }
    }
    if (L->node_of[p] < 0)
      L->node_of[p] = L->nnodes++;
  }
  L->node = L->node_of[me];

  // my node's members
  L->node_pes = malloc(npes * sizeof(int));
  assert(L->node_pes);
  for (int p = 0; p < npes; p++) {
    if (L->node_of[p] == L->node) {
      if (p == me)
        L->node_rank = L->node_size;
      L->node_pes[L->node_size++] = p;
    }
  }
  L->far_count = npes - L->node_size;

  // contiguous, equal-sized blocks?  (block placement: node n holds [n*ppn, (n+1)*ppn))
  L->contiguous = (npes % L->nnodes == 0);
  for (int p = 0; L->contiguous && p < npes; p++)
    if (L->node_of[p] != p / (npes / L->nnodes))
      L->contiguous = 0;

  // SHMEM_TEAM_SHARED must agree with the host-name grouping when it spans PEs
  LOC_STAGE("node map done");
  L->shared_team_size = shmem_team_n_pes(SHMEM_TEAM_SHARED);
  if (L->shared_team_size > 1) {
    int ok = (L->shared_team_size == L->node_size);
    for (int i = 0; ok && i < L->shared_team_size; i++) {
      int p = shmem_team_translate_pe(SHMEM_TEAM_SHARED, i, SHMEM_TEAM_WORLD);
      ok = (p >= 0 && L->node_of[p] == L->node);
    }
    if (!ok) {
      fprintf(stderr, "gtc_locality_init: rank %d: SHMEM_TEAM_SHARED (%d PEs) disagrees with "
                      "host-name grouping (%d PEs)\n", me, L->shared_team_size, L->node_size);
      abort();
    }
  }

  // can I load and store node-mates' symmetric memory directly?
  L->ptr_ok = 1;
  for (int i = 0; i < L->node_size; i++)
    if (shmem_ptr(hashes, L->node_pes[i]) == NULL)
      L->ptr_ok = 0;

  // node team: only meaningful (and only constructible with split_2d) for block placement
  LOC_STAGE("shared team and ptr done");
  if (L->contiguous && L->nnodes > 1) {
    shmem_team_t yteam = SHMEM_TEAM_INVALID;
    if (shmem_team_split_2d(SHMEM_TEAM_WORLD, npes / L->nnodes, NULL, 0, &L->node_team,
                            NULL, 0, &yteam) != 0)
      L->node_team = SHMEM_TEAM_INVALID;
    if (yteam != SHMEM_TEAM_INVALID)
      shmem_team_destroy(yteam);
  }

  LOC_STAGE("team done");
  shmem_barrier_all();
  shmem_free(mine);
  shmem_free(hashes);
  LOC_STAGE("done");
}



void gtc_locality_fini(void) {
  gtc_locality_t *L = &_c->loc;

  if (L->node_team != SHMEM_TEAM_INVALID)
    shmem_team_destroy(L->node_team);
  free(L->node_pes);
  free(L->node_of);
  memset(L, 0, sizeof(*L));
  L->node_team = SHMEM_TEAM_INVALID;
}



void gtc_locality_print(void) {
  gtc_locality_t *L = &_c->loc;

  printf("SCIOTO nodes : %d node%s, %d PEs on node %d, %s placement, shmem_ptr to node-mates %s\n",
         L->nnodes, L->nnodes == 1 ? "" : "s", L->node_size, L->node,
         L->contiguous ? "block" : "irregular", L->ptr_ok ? "yes" : "no");
}



/** random node-mate other than me, or -1 if I am alone on my node */
int gtc_random_near_peer(void) {
  gtc_locality_t *L = &_c->loc;
  int i;

  if (L->node_size < 2)
    return -1;
  i = rand() % (L->node_size - 1);
  if (i >= L->node_rank)
    i++;
  return L->node_pes[i];
}



/** random PE on another node, or -1 if there is only one node */
int gtc_random_far_peer(void) {
  gtc_locality_t *L = &_c->loc;
  int p;

  if (L->far_count == 0)
    return -1;
  do {
    p = rand() % _c->size;
  } while (L->node_of[p] == L->node);
  return p;
}



/**
 * shmem_ptr to symaddr on each node-mate, indexed like node_pes.  Entries are
 * NULL where direct access is not possible.  Caller frees the array.
 */
void **gtc_near_ptr_table(void *symaddr) {
  gtc_locality_t *L = &_c->loc;
  void **t = malloc(L->node_size * sizeof(void *));

  assert(t);
  for (int i = 0; i < L->node_size; i++)
    t[i] = shmem_ptr(symaddr, L->node_pes[i]);
  return t;
}
