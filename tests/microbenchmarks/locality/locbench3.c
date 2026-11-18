/* locbench3.c -- are CPU atomics (via shmem_ptr) and SHMEM (NIC) atomics on the
 * same word mutually atomic?  Count lost updates.  48 PEs/node, 2 nodes, XPMEM SOS. */
#include <stdio.h>
#include <stdint.h>
#include <shmem.h>

static uint64_t word __attribute__((aligned(64)));

int main(void) {
  shmem_init();
  int me = shmem_my_pe();
  const int M = 20000, K = 8;
  /* mode 0: on-node NIC  + off-node NIC   (control)
   * mode 1: on-node CPU  + off-node NIC
   * mode 2: on-node CPU  + on-node NIC
   * mode 3: on-node CPU only              (control) */
  const char *nm[4] = {"on-node NIC + off-node NIC", "on-node CPU + off-node NIC",
                       "on-node CPU + on-node NIC ", "on-node CPU only          "};
  for (int mode = 0; mode < 4; mode++) {
    word = 0; shmem_barrier_all();
    int cpu = 0, nic = 0;
    if (me >= 1 && me <= K) { if (mode == 0) nic = 1; else cpu = 1; }
    if (mode == 0 || mode == 1) { if (me >= 48 && me < 48 + K) nic = 1; }
    if (mode == 2) { if (me >= 24 && me < 24 + K) nic = 1; }
    if (mode == 3) { if (me >= 24 && me < 24 + K) cpu = 1; }
    if (cpu) { uint64_t *p = shmem_ptr(&word, 0); for (int i = 0; i < M; i++) __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST); }
    if (nic) { for (int i = 0; i < M; i++) shmem_atomic_add(&word, 1, 0); shmem_quiet(); }
    shmem_barrier_all();
    if (me == 0) {
      uint64_t got = shmem_atomic_fetch(&word, 0);
      uint64_t exp = (uint64_t)(mode == 3 ? 2*K : 2*K) * M;
      printf("%s expected %8lu got %8lu lost %8ld\n", nm[mode], (unsigned long)exp, (unsigned long)got, (long)(exp - got));
    }
    shmem_barrier_all();
  }
  shmem_finalize();
  return 0;
}
