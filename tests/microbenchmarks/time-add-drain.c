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
        double t_add_l   = 0.0;
        double t_drain_l = 0.0;
        double avg_t_add, avg_t_drain;
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

        /**** LOCAL TASK INSERTION ****/

        ARMCI_Barrier();

        if (mythread == 0)
          printf("Timing: Local task insertion (%d tasks)\n", NITER);
        
        t_add_l = gtc_wctime();
        for (i = 0; i < NITER; i++) {
#ifdef INPLACE
                task_t *mytask = gtc_inplace_create_and_add(gtc);
                void *body = gtc_task_body(mytask);
                memset(body, 0, TASK_SIZE);
#else
                void *body = gtc_task_body(task);
                memset(body, 0, TASK_SIZE);
		gtc_add(gtc, task, mythread);
#endif
        }
        t_add_l = gtc_wctime() - t_add_l;

        printf("  %d: %f sec (%f tasks/sec, %f usec/task)\n", mythread, t_add_l, NITER/t_add_l, t_add_l/NITER*10.0e6);
        MPI_Reduce(&t_add_l, &avg_t_add, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        ARMCI_Barrier();

        /**** LOCAL TASK DRAIN ****/
        
        if (mythread == 0)
          printf("Timing: Local task pool throughput\n");

        t_drain_l = gtc_wctime();
	gtc_process(gtc);
        t_drain_l = gtc_wctime() - t_drain_l;

        printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", mythread, t_drain_l, NITER/t_drain_l, t_drain_l/NITER);
        MPI_Reduce(&t_drain_l, &avg_t_drain, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
        ARMCI_Barrier();
        //gtc_print_stats(tc);
        if (mythread == 0) {
          printf("AVG ADD  : %f sec (%f tasks/sec, %e sec/task)\n", avg_t_add/nthreads,
              NITER/(avg_t_add/nthreads), (avg_t_add/nthreads)/NITER);
          printf("AVG DRAIN: %f sec (%f tasks/sec, %e sec/task)\n", avg_t_drain/nthreads,
              NITER/(avg_t_drain/nthreads), (avg_t_drain/nthreads)/NITER);
        }
        ARMCI_Barrier();
       
	gtc_task_destroy(task);
	gtc_destroy(gtc);

	ARMCI_Finalize();
	MPI_Finalize();

	return 0;
}
