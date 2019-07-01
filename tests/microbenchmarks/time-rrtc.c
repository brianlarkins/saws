#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <tc.h>
#include <armci.h>

#define NITER      100000
#define VERBOSE    0

static task_handle_t  task_handle;
static tc_t          *tc;


void task_fcn(tc_t *tc, task_t *task) {
	return;
}


int main(int argc, char **argv)
{
	int i;
        double t_add_l   = 0.0;
        double t_add_u   = 0.0;
        double t_drain_l = 0.0;
        double t_drain_u = 0.0;
	task_t *task;
       
	MPI_Init(&argc, &argv);
	ARMCI_Init();
        
	tc = gtc_create(0, NITER, NULL, MPI_COMM_WORLD);
        //gtc_disable_stealing(tc);

	task_handle = gtc_register(tc, task_fcn);
	task        = gtc_task_create(0, task_handle);

	if (tc->mythread == 0) printf("Starting task collection timing with %d threads\n", tc->nthreads);

        /**** LOCKED TASK INSERTION ****/

        // Point mythread at my neighbor rin this test with all operations being remote.
        tc->mythread = (tc->mythread + 1) % tc->nthreads;
        ARMCI_Barrier();

        if (tc->mythread == 0)
          printf("Timing: Locked task insertion (%d tasks)\n", NITER);
        
        t_add_l = gtc_wctime();
        for (i = 0; i < NITER; i++) {
		gtc_add(tc, task, tc->mythread);
        }
        t_add_l = gtc_wctime() - t_add_l;

        // Un-twiddle my thread id
        tc->mythread = (tc->mythread-1 < 0) ? tc->nthreads-1 : tc->mythread-1;
        printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", tc->mythread, t_add_l, NITER/t_add_l, t_add_l/NITER);

        // Point mythread at my neighbor so all ops will be remote.
        tc->mythread = (tc->mythread + 1) % tc->nthreads;
        ARMCI_Barrier();

        /**** LOCKED TASK DRAIN ****/
        
        if (tc->mythread == 0)
          printf("Timing: Locked task pool throughput\n");

        t_drain_l = gtc_wctime();
	gtc_process(tc);
        t_drain_l = gtc_wctime() - t_drain_l;

        // Un-twiddle my thread id
        tc->mythread = (tc->mythread-1 < 0) ? tc->nthreads-1 : tc->mythread-1;
        printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", tc->mythread, t_drain_l, NITER/t_drain_l, t_drain_l/NITER);
        ARMCI_Barrier();
        gtc_print_stats(tc);
        ARMCI_Barrier();

        /**** UNLOCKED TASK INSERTION ****/
        gtc_reset(tc);
        gtc_begin_single(tc, tc->mythread);
        // Point mythread at my neighbor so all ops will be remote.
        tc->mythread = (tc->mythread + 1) % tc->nthreads;
        ARMCI_Barrier();

        if (tc->mythread == 0)
          printf("\nTiming: Unlocked task insertion (%d tasks)\n", NITER);
        
        t_add_u = gtc_wctime();
        for (i = 0; i < NITER; i++) {
		gtc_add(tc, task, tc->mythread);
        }
        t_add_u = gtc_wctime() - t_add_u;

        // Un-twiddle my thread id
        tc->mythread = (tc->mythread-1 < 0) ? tc->nthreads-1 : tc->mythread-1;
        printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", tc->mythread, t_add_u, NITER/t_add_u, t_add_u/NITER);
        // Point mythread at my neighbor so all ops will be remote.
        tc->mythread = (tc->mythread + 1) % tc->nthreads;
        ARMCI_Barrier();

        /**** UNLOCKED TASK DRAIN ****/
        
        if (tc->mythread == 0)
          printf("Timing: Unlocked task pool throughput\n");
        
        t_drain_u = gtc_wctime();
	gtc_process(tc);
        t_drain_u = gtc_wctime() - t_drain_u;

        // Un-twiddle my thread id
        tc->mythread = (tc->mythread-1 < 0) ? tc->nthreads-1 : tc->mythread-1;
        printf("  %d: %f sec (%f tasks/sec, %e sec/task)\n", tc->mythread, t_drain_u, NITER/t_drain_u, t_drain_u/NITER);
        ARMCI_Barrier();
        gtc_print_stats(tc);
        ARMCI_Barrier();

        /**** DONE ****/
        
	gtc_task_destroy(task);
	gtc_destroy(tc);

	ARMCI_Finalize();
	MPI_Finalize();

	return 0;
}
