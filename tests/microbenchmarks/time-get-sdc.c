#include <stdio.h>
#include <unistd.h>

#include <tc.h>
#include <sdc_shr_ring.h>

#define GTC_TEST_DEBUG   1
#define CHUNKSIZE 5
#define QSIZE   16384
#define NUM     203
#define NUMREPS 100
#define TAILTARGET  (rb->procid+1)%rb->nproc /* round robin */
#define HEADTARGET  rb->procid               /* local only */

typedef struct {
  int    id;
  char   junk[100];
  int    check;
} elem_t;

int main(int argc, char **argv, char **envp) {
  int errors = 0;
  int i, dest;
  int arg, niter, ntasks, tasksize;
  char *tasks;
  sdc_shrb_t *rb;
  tc_timer_t time;
  tc_tsctimer_t tsctime;

  niter    = 5000;
  ntasks   = 10;
  tasksize = 24;

  setbuf(stdout, NULL);
  TC_INIT_ATIMER(time);
  TC_INIT_ATSCTIMER(tsctime);

  while ((arg = getopt(argc, argv, "hn:t:s:")) != -1) {
    switch (arg) {
      case 'n':
        niter = atoi(optarg);
        break;
      case 't':
        ntasks = atoi(optarg);
        break;
      case 's':
        tasksize = atoi(optarg);
        break;
      case 'h':
        eprintf("  usage: time-get [-n niter] [-t ntasks/steal] [-s tasksize]\n");
        break;
    }
  }


  // just need this to setup Portals world
  gtc_init();

  if (_c->size < 2) {
    eprintf("requires at least two processes\n");
    exit(1);
  }

  int frac = (niter / (_c->size -1)) + 1;
  tasks = calloc(frac*ntasks, tasksize); // allocate "tasks"
  assert(tasks);

  rb = sdc_shrb_create(tasksize, (frac*ntasks+1));

  if (rb->procid == 0) printf("\nPortals SDC get timing test: Started with %d threads\n", rb->nproc);


  // add niter groups of ntasks to our queue
  sdc_shrb_push_n_head(rb, _c->rank, tasks, frac*ntasks);

  sdc_shrb_release_all(rb);

  gtc_barrier();

  if (_c->rank == 0) {
    TC_START_ATIMER(time);
    TC_START_ATSCTIMER(tsctime);
    for (i = 0; i < niter; i++) {
      dest = (i % (_c->size - 1)) + 1; // never ourselves
      int ret = sdc_shrb_pop_n_tail(rb, dest, ntasks, tasks, STEAL_CHUNK);
    }
    TC_STOP_ATSCTIMER(tsctime);
    TC_STOP_ATIMER(time);
  }

  gtc_barrier();

  eprintf("%d tasks/steal %d size %.3f usec/get (tsc: %.3f)\n", ntasks, tasksize, 
      TC_READ_ATIMER_USEC(time)/(double)niter, TC_READ_ATSCTIMER_USEC(tsctime)/(double)niter);
  eprintf("%05d %.3f %d\n", ntasks, TC_READ_ATSCTIMER_USEC(tsctime)/(double)niter, tasksize);

  free(tasks);

  sdc_shrb_destroy(rb);

  if (errors) return 1;
  return 0;
}
