/* locbench2.c -- cache-line ping-pong via shmem_ptr, and contention on one target word.
 * Requires the XPMEM SOS build (shmem_ptr on-node).  48 PEs/node, 2 nodes. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <shmem.h>

static uint64_t word __attribute__((aligned(64)));
static uint64_t flag __attribute__((aligned(64)));
static double   res[128];

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec*1e6 + ts.tv_nsec/1e3; }

int main(void) {
  shmem_init();
  int me = shmem_my_pe(), np = shmem_n_pes();
  const int N = 20000;

  /* (a) ping-pong: PE0 <-> v, alternating stores through shmem_ptr on PE0's flag */
  int vs[3] = {1, 3, 24}; const char *ln[3] = {"same-CCX", "same-sock", "x-socket"};
  for (int k = 0; k < 3; k++) {
    int v = vs[k];
    flag = 0; shmem_barrier_all();
    volatile uint64_t *f = (volatile uint64_t *)shmem_ptr(&flag, 0);
    double t0 = now();
    if (me == 0) {
      for (uint64_t i = 0; i < (uint64_t)N; i++) { while (*f != 2*i) ; *f = 2*i + 1; }
    } else if (me == v) {
      for (uint64_t i = 0; i < (uint64_t)N; i++) { while (*f != 2*i + 1) ; *f = 2*i + 2; }
    }
    double t1 = now();
    if (me == 0) printf("pingpong %-10s one-way cache-line transfer %.3f us\n", ln[k], (t1 - t0) / (2.0 * N));
    shmem_barrier_all();
  }

  /* (b) contention: k initiators on node 0 (PEs 1..k) hammer PE0's word.
   *     NIC = shmem_atomic_fetch_add; CPU = __atomic via shmem_ptr.
   *     Also: k off-node initiators (PEs 48..48+k-1) with NIC atomics. */
  int ks[5] = {1, 4, 8, 24, 47};
  for (int mode = 0; mode < 3; mode++) {
    for (int ki = 0; ki < 5; ki++) {
      int k = ks[ki];
      int lo = (mode == 2) ? 48 : 1;
      int active = (me >= lo && me < lo + k && me < np);
      const int M = 5000;
      word = 0; shmem_barrier_all();
      double t0 = now();
      if (active) {
        if (mode == 1) { uint64_t *p = shmem_ptr(&word, 0); for (int i = 0; i < M; i++) __atomic_fetch_add(p, 1, __ATOMIC_ACQ_REL); }
        else           { for (int i = 0; i < M; i++) (void)shmem_atomic_fetch_add(&word, 1, 0); }
      }
      double per = active ? (now() - t0) / M : 0.0;
      shmem_barrier_all();
      double *r = shmem_malloc(sizeof(double)); *r = per;
      shmem_barrier_all();
      if (me == 0) {
        double s = 0; for (int p = lo; p < lo + k && p < np; p++) { double x; shmem_getmem(&x, r, sizeof x, p); s += x; }
        printf("contend %-14s k=%2d  mean per-op latency %7.3f us  aggregate %6.2f Mops/s  (word=%lu)\n",
               mode == 0 ? "on-node NIC" : mode == 1 ? "on-node CPU" : "off-node NIC", k, s / k, k / (s / k), (unsigned long)word);
      }
      shmem_barrier_all();
      shmem_free(r);
    }
  }
  shmem_finalize();
  return 0;
}
