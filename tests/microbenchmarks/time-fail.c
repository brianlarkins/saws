#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include <tc.h>
#include <armci.h>

#define TASK_SIZE  1024
#define NITER      100000


void task_fcn(gtc_t gtc, task_t *task) {
	return;
}


int main(int argc, char **argv)
{
	int i;
        double t_trysteal_l   = 0.0;
        double t_steal_l = 0.0;
        double avg_t_trysteal, avg_t_steal;
	task_t *task;

	task_class_t task_class;
	gtc_t  gtc;
        tc_t  *tc;
        int    mythread, nthreads;
        
	MPI_Init(&argc, &argv);
	ARMCI_Init();

	gtc = gtc_create(TASK_SIZE, NITER, NULL, MPI_COMM_WORLD);
        tc  = gtc_lookup(gtc);

        mythread = gtc_mythread(gtc);
        nthreads = gtc_nthreads(gtc);

        gtc_disable_stealing(gtc);

	task_class  = gtc_task_class_register(TASK_SIZE, task_fcn);
	task        = gtc_task_create(task_class);

	if (mythread == 0) printf("Starting task collection timing with %d threads\n", nthreads);

        ARMCI_Barrier();

        if (mythread == 0)
          printf("Timing: Trysteal\n");
        
        t_trysteal_l = gtc_wctime();
        for (i = 0; i < NITER; i++) {
          gtc_try_steal_tail(gtc, 0);
        }
        t_trysteal_l = gtc_wctime() - t_trysteal_l;

        printf("  %d: %f sec (%f attempts/sec, %f usec/attempt)\n", mythread, t_trysteal_l, NITER/t_trysteal_l, t_trysteal_l/NITER*10.0e6);
        MPI_Reduce(&t_trysteal_l, &avg_t_trysteal, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        ARMCI_Barrier();

        if (mythread == 0)
          printf("Timing: Steal\n");
        
        t_steal_l = gtc_wctime();
        for (i = 0; i < NITER; i++) {
          gtc_steal_tail(gtc, 0);
        }
        t_steal_l = gtc_wctime() - t_steal_l;

        printf("  %d: %f sec (%f attempts/sec, %f usec/attempt)\n", mythread, t_steal_l, NITER/t_steal_l, t_steal_l/NITER*10.0e6);
        MPI_Reduce(&t_steal_l, &avg_t_steal, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        ARMCI_Barrier();

	gtc_task_destroy(task);
	gtc_destroy(gtc);

	ARMCI_Finalize();
	MPI_Finalize();

	return 0;
}
