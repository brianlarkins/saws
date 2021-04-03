/** Tasktree.c -- Dynamically generate and execute a tree of tasks
 *
 * Author: James Dinan <dinan.10@osu.edu>
 * Date  : August, 2008
 *
 * An example Scioto parallel program that executes a tree of tasks.  Execution
 * begins with a single root task, this task spawns nchildren tasks, each child
 * spawns nchildren tasks, and so on until the maxdepth is reached.  We keep
 * track of the total number of tasks that were executed in a replicated local
 * object, counter.  Upon completion we perform a reduction on the counter
 * to check that the correct number of tasks were executed.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <shmem.h>

#include <tc.h>

#include "busy_wait.h"

#define PADDING    0
#define NO_INPLACE

static int me, nproc;
static task_class_t  producer_tclass, consumer_tclass;

static const double work_time = 0.001;   //  1 ms
//static const int    busy_val  = 1090000; // ~1ms on Chinook pathscale compiler with -O3
//static const int    busy_val    =  400000; // ~1ms on senna gcc compiler with -O3
//static const int    busy_val    = 109000; // ~1ms on comet with -O3

static double consumer_work_units = 10;
static double producer_work_units = 1;

static int initial_producers = 1;
static int nchildren = 10;
static int maxdepth  = 2000;
static int bouncing  = 0;
static int verbose   = 0;
static gtc_qtype_t qtype = GtcQueueSAWS;

static struct timespec psleep = { 0, 1000000L };      // 1  ms
static struct timespec csleep = { 0, 5 * 1000000L }; // 10 ms

typedef struct {
  int        parent_id;
  int        level;
  int        index;
  int        ntasks_key;
  int        nproducers_key;
  int        nconsumers_key;
  char       weight[PADDING];
} pctask_t;


/**
 * Wrapper to create tasks and enqueue them in the task collection
 *
 * @param gtc         The task collection to enqueue the task into
 * @param tclass      Portable reference to the task class
 * @param level       Level of this task in the tree
 * @param index       Index of this task in the tree
 * @param ntasks_key The CLO key that is used to look up the counter
**/
void create_task(gtc_t gtc, task_class_t tclass, int level, int index, int ntasks_key, int nproducers_key, int nconsumers_key) {
    //printf("(%d) here\n", _c->rank);
#ifdef NO_INPLACE
  task_t   *task = gtc_task_create(tclass);
#else
  task_t   *task = gtc_task_inplace_create_and_add(gtc, tclass);
#endif
  pctask_t *tt   = (pctask_t*) gtc_task_body(task);

  tt->parent_id   = me;
  tt->level       = level;
  tt->index       = index;
  tt->ntasks_key  = ntasks_key;
  tt->nproducers_key = nproducers_key;
  tt->nconsumers_key = nconsumers_key;

#ifdef NO_INPLACE
#ifdef PUSHING
  gtc_add(gtc, task, index % nproc);
#else
  gtc_add(gtc, task, me);
#endif

  if (verbose) printf("  + spawned_task (%2d, %d:%d) on %d\n", tt->parent_id, tt->level, tt->index, me);

  gtc_task_destroy(task);
#endif /* NO_INPLACE */
}


/**
 * This function implements the task and is called when the task is executed.
 *
 * @param gtc        The task collection that is being executed on
 * @param descriptor The task's descriptor (contains user supplied arguments)
**/
void producer_task_fcn(gtc_t gtc, task_t *descriptor) {
  int       i;
  pctask_t *tt   = (pctask_t *) gtc_task_body(descriptor);
  int      *ctr  = (int *) gtc_clo_lookup(gtc, tt->ntasks_key);
  int      *pctr = (int *) gtc_clo_lookup(gtc, tt->nproducers_key);

  if (tt->level < maxdepth) {
    if (bouncing)
      create_task(gtc, producer_tclass, tt->level + 1, tt->index, tt->ntasks_key, tt->nproducers_key, tt->nconsumers_key);

    for (i = 0; i < nchildren; i++)
      create_task(gtc, consumer_tclass, tt->level + 1, tt->index*nchildren + i, tt->ntasks_key, tt->nproducers_key, tt->nconsumers_key);

    if (!bouncing)
      create_task(gtc, producer_tclass, tt->level + 1, tt->index, tt->ntasks_key, tt->nproducers_key, tt->nconsumers_key);
  }

  *ctr  += 1;
  *pctr += 1;

  nanosleep(&psleep, NULL);
  //busy_wait(producer_work_units*busy_val);

  if (verbose) printf("  + Producer task (%2d, %d:%d) processed by worker %d\n",
    tt->parent_id, tt->level, tt->index, me);
}


/**
 * This function implements the task and is called when the task is executed.
 *
 * @param gtc        The task collection that is being executed on
 * @param descriptor The task's descriptor (contains user supplied arguments)
**/
void consumer_task_fcn(gtc_t gtc, task_t *descriptor) {
  pctask_t *tt   = (pctask_t *) gtc_task_body(descriptor);
  int      *ctr  = (int *) gtc_clo_lookup(gtc, tt->ntasks_key);
  int      *cctr = (int *) gtc_clo_lookup(gtc, tt->nconsumers_key);

  *ctr  += 1;
  *cctr += 1;

  //busy_wait(consumer_work_units*busy_val);
  nanosleep(&csleep, NULL);

  if (verbose) printf("  - Consumer task (%2d, %d:%d) processed by worker %d\n",
    tt->parent_id, tt->level, tt->index, me);
}


void process_args(int argc, char **argv) {
  int   arg;
  char *endptr;

  while ((arg = getopt(argc, argv, "d:n:r:p:c:i:bvhBH")) != -1) {
    switch (arg) {
    case 'd':
      maxdepth = strtol(optarg, &endptr, 10);
      if (endptr == optarg) {
        printf("Error, invalid depth: %s\n", optarg);
        exit(1);
      }
      break;

    case 'n':
      nchildren = strtol(optarg, &endptr, 10);
      if (endptr == optarg) {
        printf("Error, invalid number of children: %s\n", optarg);
        exit(1);
      }
      break;

    case 'i':
      initial_producers = strtol(optarg, &endptr, 10);
      if (endptr == optarg) {
        printf("Error, invalid number of initial producers: %s\n", optarg);
        exit(1);
      }
      break;

    case 'p':
      producer_work_units = strtod(optarg, &endptr);
      if (endptr == optarg) {
        printf("Error, invalid work ratio: %s\n", optarg);
        exit(1);
      }
      break;

    case 'c':
      consumer_work_units = strtod(optarg, &endptr);
      if (endptr == optarg) {
        printf("Error, invalid work ratio: %s\n", optarg);
        exit(1);
      }
      break;

    case 'b':
      bouncing = 1;
      break;

    case 'v':
      verbose = 1;
      break;

    case 'B':
      qtype = GtcQueueSDC;
      break;
    case 'H':
      qtype = GtcQueueSAWS;
      break;

    case 'h':
      if (me == 0) {
        printf("SCIOTO Producer-Consumer Microbenchmark\n");
        printf("  Usage: %s [args]\n\n", basename(argv[0]));
        printf("Options: (flag, argument type, default value)\n");
        printf("  -d int   %5d  Max depth\n", maxdepth);
        printf("  -n int   %5d  Number of children per node\n", nchildren);
        printf("  -i int   %5d  Number of initial producers\n", initial_producers);
        printf("  -p dbl   %5.2f  Producer work size (units of %.2f ms)\n", producer_work_units, work_time);
        printf("  -c dbl   %5.2f  Consumer work size (units of %.2f ms)\n", consumer_work_units, work_time);
        printf("  -b              Enable bouncing mode\n");
        printf("  -v              Enable verbose output\n");
        printf("  -h              Help\n");
      }
      exit(0);
      break;

    default:
      if (me == 0) printf("Try '-h' for help.\n");
      exit(1);
    }
  }
}


int main(int argc, char **argv) {
  int           expected_ntasks;     // Used to check the final result
  int           expected_nproducers;
  int           expected_nconsumers;
  double        ideal_walltime;
  gtc_t         gtc;              // Portable reference to the task collection
  int           ntasks_key;       // Portable references to common local copies of the counters
  int           nproducers_key;
  int           nconsumers_key;
  tc_timer_t   time;
  static int    ntasks     = 0;   // Count the number of tasks executed
  static int    nproducers = 0;   // Count the number of producer tasks executed
  static int    nconsumers = 0;   // Count the number of consumer tasks executed
  static int    final_ntasks;     // Collective sum of everyone's stats
  static int    final_nproducers;
  static int    final_nconsumers;

  setenv("SCIOTO_DISABLE_PERNODE_STATS", "1", 1);
  //setenv("GTC_RECLAIM_FREQ", "10", 1);

  gtc_init();
  me    = _c->rank;
  nproc = _c->size;

  process_args(argc, argv);

  expected_nproducers = initial_producers*(maxdepth + 1);
  expected_nconsumers = initial_producers*(maxdepth*nchildren);
  expected_ntasks     = expected_nproducers + expected_nconsumers;
  ideal_walltime      = (expected_nproducers*work_time*producer_work_units + expected_nconsumers*work_time*consumer_work_units)/nproc;

  if (me == 0) {
    printf("SCIOTO Producer-Consumer uBench starting with %d threads\n", nproc);
    printf("-----------------------------------------------------------------------------\n\n");
    printf("Max depth = %d, nchildren = %d, producer tasks = %7d, consumer tasks = %7d\n", maxdepth, nchildren,
        expected_nproducers, expected_nconsumers);
    printf("Work unit size = %.2f ms, Producer work units = %.2f, Consumer work units = %.2f\n", work_time * 1000.0,
        producer_work_units, consumer_work_units);
    printf("Ideal Walltime = %f sec, %0.2f tasks/sec (%0.2f tasks/sec/process)\n\n", ideal_walltime,
        expected_ntasks/ideal_walltime, expected_ntasks/ideal_walltime/nproc);
  }

  gtc = gtc_create(sizeof(pctask_t), 10, bouncing ? 2*(initial_producers+nchildren) : expected_ntasks, NULL, qtype);

  ntasks_key     = gtc_clo_associate(gtc, &ntasks);     // Collectively register ntasks counter
  nproducers_key = gtc_clo_associate(gtc, &nproducers); // Collectively register nproducers counter
  nconsumers_key = gtc_clo_associate(gtc, &nconsumers); // Collectively register ncomsumers counter

  producer_tclass = gtc_task_class_register(sizeof(pctask_t), producer_task_fcn); // Collectively create a task class
  consumer_tclass = gtc_task_class_register(sizeof(pctask_t), consumer_task_fcn); // Collectively create a task class

  // Add the initial producer tasks to the task collection
  // FIXME: these are going to all get stuck on one process due to SMP-awareness
  if (me == 0) {
    int i;
    for (i = 0; i < initial_producers; i++)
      create_task(gtc, producer_tclass, 0, i, ntasks_key, nproducers_key, nconsumers_key);
  }

  // Set by hand above
  // busy_val = tune_busy_wait(work_time);
  // printf("Busy_val = %d\n", busy_val);

  if (me == 0) {
    if (bouncing)
      printf("Bouncing Producer-Consumer test starting...\n");
    else
      printf("Producer-Consumer test starting...\n");
  }

  // Process the task collection
  TC_INIT_ATIMER(time);
  TC_START_ATIMER(time);
  gtc_process(gtc);
  TC_STOP_ATIMER(time);

  // printf("(%d) ntasks: %d, nproducers: %d, nconsumers: %d\n", me, ntasks, nproducers, nconsumers);

  // Check if the correct number of tasks were processed
  shmem_sum_reduce(SHMEM_TEAM_WORLD, &final_ntasks, &ntasks, 1);
  shmem_sum_reduce(SHMEM_TEAM_WORLD, &final_nproducers, &nproducers, 1);
  shmem_sum_reduce(SHMEM_TEAM_WORLD, &final_nconsumers, &nconsumers, 1);

  if (me == 0) {
    printf("\n");
    printf("Total tasks processed = %7d, expected = %7d: %s\n", final_ntasks, expected_ntasks,
      (final_ntasks == expected_ntasks) ? "SUCCESS" : "FAILURE");
    printf("Total producer tasks  = %7d, expected = %7d: %s\n", final_nproducers, expected_nproducers,
      (final_nproducers == expected_nproducers) ? "SUCCESS" : "FAILURE");
    printf("Total consumer tasks  = %7d, expected = %7d: %s\n", final_nconsumers, expected_nconsumers,
      (final_nconsumers == expected_nconsumers) ? "SUCCESS" : "FAILURE");
    double atime = TC_READ_ATIMER_SEC(time); // was having conversion issues, probably fixed now, but whatever
    printf("Actual Walltime = %f sec, %0.2f tasks/sec (%0.2f tasks/sec/process)\n", TC_READ_ATIMER_SEC(time),
      final_ntasks/atime, final_ntasks/atime/nproc);
    printf(" Ideal Walltime = %f sec, %0.2f tasks/sec (%0.2f tasks/sec/process)\n\n", ideal_walltime,
      final_ntasks/ideal_walltime, final_ntasks/ideal_walltime/nproc);
    printf("\n");
  }

  shmem_barrier_all();
  gtc_print_stats(gtc);
  shmem_barrier_all();
  gtc_destroy(gtc);
  gtc_fini();

  return 0;
}

