#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <tc.h>
#include <termination.h>

double dummy = 0.0;

int main(int argc, char **argv)
{
  int i, comm_size, comm_rank, NITER = 1;
  int NBARRIER = 1000, ret = 0;
  double t_td = 0.0;
  double t_armci_barrier = 0.0;
  double t_td_max, t_armci_max;
  td_t **tds;
  tc_timer_t tdtime, barriertime;

  if (argc > 1) {
    NITER = atoi(argv[1]);
  }

  if ((argc <= 1) || (NITER <= 0) || (NITER >= 512)) {
    gtc_init();
    eprintf("Usage: %s NITER (512 max)\n", argv[0]);
    ret = 1;
    goto done;
  } else {
    setenv("GTC_MAX_PTES", argv[1], 1);
  }

  gtc_init();

  comm_rank = _c->rank;
  comm_size = _c->size;


  TC_INIT_ATIMER(tdtime);
  TC_INIT_ATIMER(barriertime);

  if (comm_rank == 0) printf("Termination Detection uBench -- NITER = %d, NPROC = %d\n\n", NITER, comm_size);


  if (comm_rank == 0) printf("Performing termination detection timing...\n");
  fflush(NULL);
  tds = calloc(NITER, sizeof(td_t));
  for (int i=0; i<NITER; i++) 
    tds[i] = td_create();
  gtc_barrier();

  //t_td = MPI_Wtime();
  TC_START_ATIMER(tdtime);
  for (i = 0; i < NITER; i++) {
    while (!td_attempt_vote(tds[i]));
    // MPI_Barrier(MPI_COMM_WORLD); // Prevent from overlapping
    gtc_barrier();
  }
  //t_td = MPI_Wtime() - t_td;
  TC_STOP_ATIMER(tdtime);
  for (int i=0; i<NITER; i++)
    td_destroy(tds[i]);
  free(tds);


  if (comm_rank == 0) printf("Performing gtc_barrier() timing...\n");
  fflush(NULL);
  gtc_barrier();

  //t_armci_barrier = MPI_Wtime();
  TC_START_ATIMER(barriertime);
  for (i = 0; i < NBARRIER; i++) {
    gtc_barrier();
  }
  //t_armci_barrier = MPI_Wtime() - t_armci_barrier;
  TC_STOP_ATIMER(barriertime);

  t_td            = TC_READ_ATIMER_MSEC(tdtime);
  t_armci_barrier = TC_READ_ATIMER_MSEC(barriertime);

  gtc_reduce(&t_td, &t_td_max,               GtcReduceOpMax, DoubleType, 1);
  gtc_reduce(&t_armci_barrier, &t_armci_max, GtcReduceOpMax, DoubleType, 1);

  double perbarrier = t_armci_max / (double)NBARRIER;
  double final = t_td_max - (perbarrier * (double)NITER);
  if (comm_rank == 0) {
    printf("\nResults: %0.9f ms/td td+barr: %0.9f ms/td, barr: %0.9f ms/barrier\n",
        final/NITER, t_td_max/NITER, t_armci_max/NBARRIER);
    printf("%04d   %0.9f  %0.9f  %0.9f\n", _c->size, final/NITER, t_td_max/NITER, t_armci_max/NBARRIER);
  }

done:
  gtc_fini();

  return ret;
}
