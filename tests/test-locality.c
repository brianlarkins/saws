/*
 * test-locality.c -- checks for libtc node discovery and local-first target selection.
 *
 *   srun --mpi=pmix -N <nodes> --ntasks-per-node=<ppn> ./test-locality
 *
 * Prints "Locality test: SUCCESS" or "Locality test: FAILURE (<n> errors)" on rank 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <tc.h>

#define HOSTWORDS 8      // 64-byte host names
#define NPICKS    20000

static int errors = 0;
static int trace = 0;
#define STAGE(s) do { if (trace) fprintf(stderr, "test rank %d: %s\n", _c->rank, s); } while (0)

#define CHECK(cond, ...) do { if (!(cond)) { errors++; if (errors <= 5) { \
      printf("rank %d: FAILED: ", _c->rank); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static int in_node(int pe) {
  for (int i = 0; i < _c->loc.node_size; i++)
    if (_c->loc.node_pes[i] == pe) return 1;
  return 0;
}

int main(int argc, char **argv) {
  UNUSED(argc); UNUSED(argv);
  gtc_init();
  trace = getenv("GTC_DEBUG_LOCALITY") != NULL;
  STAGE("gtc_init done");

  gtc_locality_t *L = &_c->loc;
  int me = _c->rank, npes = _c->size;

  // 1. node membership agrees with the host names
  uint64_t *myhost = shmem_calloc(HOSTWORDS, sizeof(uint64_t));
  uint64_t *hosts  = shmem_calloc((size_t)npes * HOSTWORDS, sizeof(uint64_t));
  gethostname((char *)myhost, HOSTWORDS * sizeof(uint64_t) - 1);
  shmem_barrier_all();
  shmem_uint64_fcollect(SHMEM_TEAM_WORLD, hosts, myhost, HOSTWORDS);
  for (int p = 0; p < npes; p++) {
    int same_host = !memcmp(&hosts[p*HOSTWORDS], myhost, HOSTWORDS * sizeof(uint64_t));
    CHECK(same_host == gtc_is_near(p), "PE %d: same host %d but near %d", p, same_host, gtc_is_near(p));
  }
  CHECK(L->node_pes[L->node_rank] == me, "node_pes[node_rank] = %d, not me", L->node_pes[L->node_rank]);
  CHECK(L->node_size + L->far_count == npes, "node_size %d + far %d != npes", L->node_size, L->far_count);

  STAGE("1 hosts done");
  // 2. every PE has the same node map as PE 0
  int *map = shmem_malloc(npes * sizeof(int)), *map0 = malloc(npes * sizeof(int));
  memcpy(map, L->node_of, npes * sizeof(int));
  shmem_barrier_all();
  shmem_getmem(map0, map, npes * sizeof(int), 0);
  CHECK(!memcmp(map0, L->node_of, npes * sizeof(int)), "node map differs from PE 0's");

  STAGE("2 map done");
  // 3. shmem_ptr table reaches each node-mate's copy of a symmetric variable
  int *sym = shmem_malloc(sizeof(int));
  *sym = me;
  shmem_barrier_all();
  void **tab = gtc_near_ptr_table(sym);
  int reached = 0;
  for (int i = 0; i < L->node_size; i++) {
    if (tab[i]) {
      reached++;
      CHECK(*(volatile int *)tab[i] == L->node_pes[i], "ptr to PE %d reads %d", L->node_pes[i], *(int *)tab[i]);
    }
  }
  if (L->ptr_ok)
    CHECK(reached == L->node_size, "ptr_ok but only %d of %d node-mates reachable", reached, L->node_size);
  free(tab);

  STAGE("3 ptr done");
  // 4. random peer pickers stay in scope
  for (int k = 0; k < NPICKS; k++) {
    int n = gtc_random_near_peer(), f = gtc_random_far_peer();
    if (L->node_size > 1) CHECK(n != me && in_node(n), "near pick %d out of scope", n);
    else                  CHECK(n == -1, "near pick %d with no node-mates", n);
    if (L->far_count > 0) CHECK(f >= 0 && !in_node(f), "far pick %d out of scope", f);
    else                  CHECK(f == -1, "far pick %d with one node", f);
  }

  // 5. target policies through gtc_select_target
  gtc_ldbal_cfg_t cfg;
  gtc_ldbal_cfg_init(&cfg);
  STAGE("4 pickers done");
  gtc_t gtc = gtc_create(64, 10, 1000, &cfg, GtcQueueSAWS);

  //   5a. local-prob at 75%: about 75% node-mates when both scopes exist
  cfg.target_selection = TARGET_LOCAL_PROB;
  cfg.local_search_factor = 75;
  gtc_ldbal_cfg_set(gtc, &cfg);
  gtc_vs_state_t st;
  memset(&st, 0, sizeof(st));
  int nnear = 0;
  for (int k = 0; k < NPICKS; k++) {
    int v = gtc_select_target(gtc, &st);
    CHECK(v >= 0 && v < npes && v != me, "local-prob picked %d", v);
    nnear += gtc_is_near(v);
  }
  if (L->node_size > 1 && L->far_count > 0) {
    double frac = nnear / (double)NPICKS;
    CHECK(frac > 0.72 && frac < 0.78, "local-prob 75%%: near fraction %.3f", frac);
  }

  //   5b. local-sweep with 3 far attempts: node-mates once each, then 3 far, then again
  cfg.target_selection = TARGET_LOCAL_SWEEP;
  cfg.local_sweep_far = 3;
  gtc_ldbal_cfg_set(gtc, &cfg);
  memset(&st, 0, sizeof(st));
  if (L->node_size > 1) {
    for (int round = 0; round < 3; round++) {
      char *seen = calloc(npes, 1);
      for (int k = 0; k < L->node_size - 1; k++) {
        int v = gtc_select_target(gtc, &st);
        CHECK(v != me && in_node(v) && !seen[v], "sweep round %d step %d picked %d", round, k, v);
        if (v >= 0 && v < npes) seen[v] = 1;
      }
      free(seen);
      if (L->far_count > 0) {
        for (int k = 0; k < 3; k++) {
          int v = gtc_select_target(gtc, &st);
          CHECK(v >= 0 && !in_node(v), "sweep round %d far step %d picked %d", round, k, v);
        }
      }
    }
  }

  STAGE("5 policies done");
  // collect results
  int *err = shmem_malloc(sizeof(int)), *tot = shmem_malloc(sizeof(int));
  *err = errors;
  shmem_barrier_all();
  shmem_int_sum_reduce(SHMEM_TEAM_WORLD, tot, err, 1);
  if (me == 0) {
    printf("nodes %d, PEs per node %d (node 0), ptr_ok %d, shared team %d\n",
           L->nnodes, L->node_size, L->ptr_ok, L->shared_team_size);
    if (*tot == 0) printf("Locality test: SUCCESS\n");
    else           printf("Locality test: FAILURE (%d errors)\n", *tot);
  }

  STAGE("results done");
  gtc_destroy(gtc);
  STAGE("destroy done");
  gtc_fini();
  return 0;
}
