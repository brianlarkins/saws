#include <stdio.h>
#include <unistd.h>

#include <tc.h>
#include <sn_ring.h>

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


void print_queue(sn_ring_t *rb) {
  elem_t *ela = (elem_t *)rb->q;

  sn_print(rb);
  for (int i=0; i<QSIZE; i++)
    printf("%d: q[%d] = %d :: %d\n", _c->rank, i, ela[i].id, ela[i].check);
}


void print_buf(elem_t *a) {
  for (int i=0; i<NUM; i++)
    printf("%d: q[%d] = %d :: %d\n", _c->rank, i, a[i].id, a[i].check);
}



int main(int argc, char **argv, char **envp) {
  int errors = 0;
  int i, dest;
  int arg, niter, ntasks, tasksize;
  char *tasks;
  sn_ring_t *rb;
  tc_timer_t time;
  tc_tsctimer_t tsctime;

  niter    = 10000;
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
  tasks = calloc(frac*ntasks, tasksize); // allocate "tasks" - only need NITER/P + 1
  assert(tasks);

  rb = sn_create(tasksize, frac*ntasks, 5);

  if (rb->procid == 0) printf("\nPortals Steal-Half get timing test: Started with %d threads\n", rb->nproc);

  // add niter groups of ntasks to our queue
  sn_push_n_head(rb, _c->rank, tasks, frac*ntasks);

  sh_release_all_chunks(rb, ntasks);

  gtc_barrier();

  if (_c->rank == 0) {
    TC_START_ATIMER(time);
    TC_START_ATSCTIMER(tsctime);
    for (i = 0; i < niter; i++) {
      dest = (i % (_c->size - 1)) + 1; // never ourselves
      int ret = sh_pop_n_tail(rb, dest, ntasks, tasks, 0);
    }
    TC_STOP_ATSCTIMER(tsctime);
    TC_STOP_ATIMER(time);
  }


  gtc_barrier();
  eprintf("%d tasks/steal %d size %.3f usec/get (tsc: %.3f)\n", ntasks, tasksize, 
      TC_READ_ATIMER_USEC(time)/(double)niter, TC_READ_ATSCTIMER_USEC(tsctime)/(double)niter);
  eprintf("TSC: poptail %.3f pre: %.3f wait: %.3f\n", 
      TC_READ_TSCTIMER_USEC(poptail)/(double)niter, TC_READ_TSCTIMER_USEC(addinplace)/(double)niter,
      TC_READ_TSCTIMER_USEC(addfinish)/(double)niter);
  eprintf("%05d %.3f %d\n", ntasks, TC_READ_ATSCTIMER_USEC(tsctime)/(double)niter, tasksize);

  free(tasks);

  sn_destroy(rb);

  if (errors) return 1;
  return 0;
}
