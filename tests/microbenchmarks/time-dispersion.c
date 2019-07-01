#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <tc.h>
#include <armci.h>

#define TASK_SIZE  1024

void split_shrb_release_everything(struct split_shrb_s *rb);

void task_fcn(gtc_t gtc, task_t *task) {
}


int main(int argc, char **argv)
{
	int     i;
        double  t_add = 0.0, t_disperse = 0.0;
	task_t *task;

	task_class_t task_class;
	gtc_t  gtc;
        tc_t  *tc;
        int    mythread, nthreads;
        
	MPI_Init(&argc, &argv);
	ARMCI_Init();

        MPI_Comm_size(MPI_COMM_WORLD, &nthreads);
        MPI_Comm_rank(MPI_COMM_WORLD, &mythread);

	gtc = gtc_create(TASK_SIZE, nthreads, NULL, MPI_COMM_WORLD);
        tc  = gtc_lookup(gtc);

	task_class  = gtc_task_class_register(task_fcn);
	task        = gtc_task_create_ofclass(TASK_SIZE, task_class);

	if (mythread == 0) printf("Starting dispersion timing with %d threads\n", nthreads);

        /**** CREATE TASKS ON PROC 0 ****/

        if (mythread == 0) {
          printf("Thread 0: Creating %d tasks\n", nthreads);

          t_add = gtc_wctime();

          for (i = 0; i < nthreads; i++)
            gtc_add(gtc, task, mythread);

          t_add = gtc_wctime() - t_add;

          printf("Thread 0: %f sec (%f tasks/sec, %e sec/task)\n", mythread, t_add, nthreads/t_add, t_add/nthreads);
        }
        
        ARMCI_Barrier();

        /**** TIME FIRST ACQUIRE ****/
        
        if (mythread == 0)
          printf("Timing: First acquire\n");

        t_disperse = gtc_wctime();

        gtc_get_buf(gtc, 0, task);
        split_shrb_release_everything(tc->shared_rb);
        
        t_disperse = gtc_wctime() - t_disperse;

        printf("  %d: %f ms\n", mythread, t_disperse*1000.0);
        ARMCI_Barrier();
       
	gtc_task_destroy(task);
	gtc_destroy(gtc);

	ARMCI_Finalize();
	MPI_Finalize();

	return 0;
}
