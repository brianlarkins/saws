#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include <tc.h>
#include <armci.h>

#define NITER      100000


void task_fcn(gtc_t gtc, task_t *task) {
	return;
}


int main(int argc, char **argv)
{
	int i, task_size;
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

        if (argc < 2) {
          printf("Usage: %s task_size\n", argv[0]);
          ARMCI_Finalize();
          MPI_Finalize();
          return 1;
        }

        task_size = atoi(argv[1]);

	gtc = gtc_create(task_size, NITER, NULL, MPI_COMM_WORLD);
        tc  = gtc_lookup(gtc);

        mythread = gtc_mythread(gtc);
        nthreads = gtc_nthreads(gtc);

        gtc_disable_stealing(gtc);

	task_class  = gtc_task_class_register(task_size, task_fcn);

	if (mythread == 0) printf("Starting task collection timing with %d threads\n", nthreads);
	if (mythread == 0) printf("Task body size = %d, NITER = %d\n", task_size, NITER);

        /**** LOCAL TASK INSERTION ****/

        ARMCI_Barrier();

        if (mythread == 0)
          printf("Timing: Local task insertion (%d tasks)\n", NITER);
        
        t_add_l = gtc_wctime();
        for (i = 0; i < NITER; i++) {
#ifdef INPLACE
                task_t *mytask = gtc_task_inplace_create_and_add(gtc, task_class);
                void *body     = gtc_task_body(mytask);
                memset(body, 0, task_size);
                //gtc_task_inplace_create_and_add_finish(gtc, mytask);
#else
                task_t *task = gtc_task_create(task_class);
                void *body   = gtc_task_body(task);
                memset(body, 0, task_size);
		gtc_add(gtc, task, mythread);
                gtc_task_destroy(task);
#endif
        }
        t_add_l = gtc_wctime() - t_add_l;

        printf("  %d: %f sec (%f tasks/sec, %f usec/task)\n", mythread, t_add_l, NITER/t_add_l, t_add_l/NITER*10.0e6);
        
        if (mythread == 0) printf("!! %d %f\n", task_size, t_add_l/NITER*10.0e6);

	gtc_destroy(gtc);

	ARMCI_Finalize();
	MPI_Finalize();

	return 0;
}
