/*
 * locbench.c -- per-locality-level latency of SAWS steal primitives.
 * PE 0 is the initiator; targets at: same CCX (PE 1), same socket other CCX (PE 3),
 * other socket (PE 24), other node (PE 48).  Expects 48 PEs/node, block layout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <shmem.h>

#define ITERS  20000
#define WARMUP 1000
#define MAXB   (64*1024)

static uint64_t sv;          /* stand-in for steal_val     */
static int64_t  status[64];  /* stand-in for completion array */
static char     q[MAXB];     /* stand-in for task queue    */

static double now(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int cmpd(const void *a, const void *b) {
  double x = *(double*)a, y = *(double*)b; return (x > y) - (x < y);
}

static double *lat;

static void report(const char *lvl, const char *op, int n) {
  qsort(lat, n, sizeof(double), cmpd);
  double s = 0; for (int i = 0; i < n; i++) s += lat[i];
  printf("%-10s %-22s mean %7.3f  p50 %7.3f  p99 %7.3f us\n", lvl, op, s/n, lat[n/2], lat[(int)(n*0.99)]);
}

int main(void) {
  shmem_init();
  int me = shmem_my_pe(), np = shmem_n_pes();
  static char buf[MAXB];
  lat = malloc(sizeof(double) * ITERS);

  int nshared = shmem_team_n_pes(SHMEM_TEAM_SHARED);
  if (me == 0) {
    printf("npes %d  SHMEM_TEAM_SHARED size %d\n", np, nshared);
    int ptrok = 0;
    for (int p = 0; p < np; p++) if (p != me && shmem_ptr(&sv, p)) ptrok++;
    printf("shmem_ptr non-NULL for %d of %d remote PEs\n", ptrok, np - 1);
  }

  /* team construction checks */
  shmem_team_t sock = SHMEM_TEAM_INVALID, node = SHMEM_TEAM_INVALID;
  int ppn = 48;
  shmem_team_split_strided(SHMEM_TEAM_WORLD, (me / 24) * 24, 1, 24, NULL, 0, &sock);
  shmem_team_split_strided(SHMEM_TEAM_WORLD, (me / ppn) * ppn, 1, ppn, NULL, 0, &node);
  if (me == 0)
    printf("socket team size %d  node team size %d  translate(node rank 5 -> world) %d\n",
           sock == SHMEM_TEAM_INVALID ? -1 : shmem_team_n_pes(sock),
           node == SHMEM_TEAM_INVALID ? -1 : shmem_team_n_pes(node),
           node == SHMEM_TEAM_INVALID ? -1 : shmem_team_translate_pe(node, 5, SHMEM_TEAM_WORLD));
  shmem_barrier_all();

  int targets[4]  = {1, 3, 24, 48};
  const char *lvl[4] = {"same-CCX", "same-sock", "x-socket", "x-node"};
  int sizes[4] = {64, 512, 4096, 32768};

  if (me == 0) {
    for (int v = 0; v < 4; v++) {
      int t = targets[v];
      if (t >= np) continue;
      void *rp = shmem_ptr(&sv, t);

      for (int i = -WARMUP; i < ITERS; i++) { double a = now();
        (void)shmem_atomic_fetch(&sv, t); double b = now(); if (i >= 0) lat[i] = b - a; }
      report(lvl[v], "atomic_fetch", ITERS);

      for (int i = -WARMUP; i < ITERS; i++) { double a = now();
        (void)shmem_atomic_fetch_add(&sv, 1, t); double b = now(); if (i >= 0) lat[i] = b - a; }
      report(lvl[v], "atomic_fetch_add", ITERS);

      if (rp) {
        for (int i = -WARMUP; i < ITERS; i++) { double a = now();
          (void)__atomic_fetch_add((uint64_t*)rp, 1, __ATOMIC_ACQ_REL); double b = now(); if (i >= 0) lat[i] = b - a; }
        report(lvl[v], "shmem_ptr fetch_add", ITERS);
      }

      for (int s = 0; s < 4; s++) {
        char nm[32]; snprintf(nm, 32, "getmem %dB", sizes[s]);
        for (int i = -WARMUP; i < ITERS; i++) { double a = now();
          shmem_getmem(buf, q, sizes[s], t); double b = now(); if (i >= 0) lat[i] = b - a; }
        report(lvl[v], nm, ITERS);
      }

      /* full SAWS steal: claim + nbi get + quiet + completion add */
      for (int s = 0; s < 2; s++) {
        int sz = s == 0 ? 512 : 4096;
        char nm[32]; snprintf(nm, 32, "saws-steal %dB", sz);
        for (int i = -WARMUP; i < ITERS; i++) { double a = now();
          (void)shmem_atomic_fetch_add(&sv, 1, t);
          shmem_getmem_nbi(buf, q, sz, t);
          shmem_quiet();
          shmem_atomic_add(&status[i & 63], 1, t);
          double b = now(); if (i >= 0) lat[i] = b - a; }
        report(lvl[v], nm, ITERS);
      }
      /* include the trailing quiet that makes the completion visible */
      for (int i = -WARMUP; i < ITERS; i++) { double a = now();
        (void)shmem_atomic_fetch_add(&sv, 1, t);
        shmem_getmem_nbi(buf, q, 512, t);
        shmem_quiet();
        shmem_atomic_add(&status[i & 63], 1, t);
        shmem_quiet();
        double b = now(); if (i >= 0) lat[i] = b - a; }
      report(lvl[v], "saws-steal 512B+quiet", ITERS);
    }
  }
  shmem_barrier_all();
  shmem_finalize();
  return 0;
}
