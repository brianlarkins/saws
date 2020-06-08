#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

#include <mpi.h>
#include <armci.h>

#define NITER 1000

int mythread, nthreads;

int main(int argc, char **argv) {
  int    i, j;
  double timer;
  int  **mutexes;
  
  MPI_Init(&argc, &argv);
  ARMCI_Init();

  MPI_Comm_rank(MPI_COMM_WORLD, &mythread);
  MPI_Comm_size(MPI_COMM_WORLD, &nthreads);

  mutexes = malloc(nthreads*sizeof(int*));
  ARMCI_Malloc((void**)mutexes, sizeof(int));

  *mutexes[mythread] = 0;

  if (mythread == 0)
    printf("Counter test starting on %d processes\n", nthreads);

  timer = MPI_Wtime();
  ARMCI_Barrier();
  
  for (i = 0; i < NITER; i++) {
    for (j = 0; j < nthreads; j++) {
      int swap_val;
      /** LOCK */
      do {
        swap_val = 1;
        ARMCI_Rmw(ARMCI_SWAP, &swap_val, mutexes[j], 0, j);
        if (swap_val != 1 && swap_val != 0)
          printf(" -- Thread %d error: expected 1 or 0, got %d on mutex %d\n", mythread, swap_val, j);
      } while (swap_val != 0);
      
      /** UNLOCK */
      swap_val = 0;
      ARMCI_Rmw(ARMCI_SWAP, &swap_val, mutexes[j], 0, j);
      if (swap_val != 1)
        printf(" -- Thread %d error: expected 1, got %d on mutex %d\n", mythread, swap_val, j);
    }
  }

  ARMCI_Barrier();
  timer = MPI_Wtime() - timer;

  if (*mutexes[mythread] != 0)
    printf(" -- Thread %d error: my counter was %d, not 0\n", mythread, *mutexes[mythread]);

  if (mythread == 0)
    printf("Counter test completed %d rmw ops in %f sec\n", 2*NITER*nthreads, timer);

  ARMCI_Finalize();
  MPI_Finalize();

  return 0;
}
