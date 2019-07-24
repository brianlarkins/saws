/*
 * Copyright (C) 2018. See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <alloca.h>
#include <math.h>

#include <tc.h>

void gtc_print_my_stats(gtc_t gtc);
//static int dcomp(const void *a, const void *b);

enum victim_types_e { FREE, LOCAL_SEARCH, RETRY };

char *victim_methods[2] = { "Random", "Round Robin" };
char *steal_methods[3]  = { "Half", "Greedy", "Chunk" };

static int gtc_is_seeded = 0;

/**
 * Create a new task collection.  Collective call.
 *
 * @param max_body_size  IN Max size of a task descriptor's body in bytes for this tc.  Any task   that is
 *                      inserted must be of this size.  For tasks with descriptors smaller
 *                      than task_size they should be placed in buffers of task_size in length.
 * @param chunk_size IN Number of tasks to be transferred as the result of a steal operation.
 *
 * @return           pointer to task collection handle
 */
gtc_t gtc_create(int max_body_size, int chunk_size, int shrb_size, gtc_ldbal_cfg_t *ldbal_cfg, gtc_qtype_t qtype) {
  gtc_t     gtc;
  tc_t     *tc;

  // baseline
  // aborting steals and steal half with split queues

  if (!gtc_is_initialized) {
    gtc_is_initialized = -1; // signify that we are initializing sciotwo implicitly
    gtc_init();
  }

  _c->total_tcs++;

  if (!gtc_is_seeded) {
    struct timespec t = gtc_get_wtime();
    unsigned rseed = (1000000000L * t.tv_sec) + t.tv_nsec;
    srand(rseed + _c->rank); // seed randomness (for stealing)
    gtc_is_seeded = 1;
  }

  // allocate collection
  tc = calloc(1, sizeof(tc_t));
  assert(tc != NULL);

  // add to global registry
  gtc = gtc_handle_register(tc);


  // allocate timers
  tc->timers = calloc(1,sizeof(tc_timers_t));
  TC_INIT_TIMER(tc,process);
  TC_INIT_TIMER(tc,passive);
  TC_INIT_TIMER(tc,search);
  TC_INIT_TIMER(tc,active);
  TC_INIT_TIMER(tc,steal);
  TC_INIT_TIMER(tc,put);
  TC_INIT_TIMER(tc,get);
  TC_INIT_TIMER(tc,dispersion);
  TC_INIT_TIMER(tc,imbalance);
  for (int i=0; i<5; i++)
    TC_INIT_TIMER(tc,t[i]);

/* TEST CODE */
  tc->tsctimers = calloc(1, sizeof(tc_tsctimers_t));
  TC_INIT_TSCTIMER(tc, getbuf);
  TC_INIT_TSCTIMER(tc, add);
  TC_INIT_TSCTIMER(tc, addinplace);
  TC_INIT_TSCTIMER(tc, addfinish);
  TC_INIT_TSCTIMER(tc, progress);
  TC_INIT_TSCTIMER(tc, reclaim);
  TC_INIT_TSCTIMER(tc, ensure);
  TC_INIT_TSCTIMER(tc, release);
  TC_INIT_TSCTIMER(tc, reacquire);
  TC_INIT_TSCTIMER(tc, pushhead);
  TC_INIT_TSCTIMER(tc, poptail);
  TC_INIT_TSCTIMER(tc, getsteal);
  TC_INIT_TSCTIMER(tc, getfail);
  TC_INIT_TSCTIMER(tc, getmeta);
  TC_INIT_TSCTIMER(tc, sanity);
/* TEST CODE */

  if (!ldbal_cfg) {
    ldbal_cfg = alloca(sizeof(gtc_ldbal_cfg_t));
    gtc_ldbal_cfg_init(ldbal_cfg);
  }

  switch (qtype) {
    case GtcQueueSDC:
      break;
    case GtcQueuePortalsN:
      ldbal_cfg->chunk_size = chunk_size;
      ldbal_cfg->steal_method = STEAL_CHUNK;
      break;
    case GtcQueuePortalsHalf:
      ldbal_cfg->steal_method = STEAL_HALF;
      break;
    default:
      gtc_eprintf(DBGERR, "gtc_create: unsupported queue type\n");
      exit(1);
      break;
  }


  if (max_body_size == AUTO_BODY_SIZE)
    max_body_size = gtc_task_class_largest_body_size();

  if (ldbal_cfg->steal_method == STEAL_CHUNK) {
    tc->steal_buf = malloc(ldbal_cfg->chunk_size*(sizeof(task_t)+max_body_size));
  } else {
    tc->steal_buf = malloc(__GTC_MAX_STEAL_SIZE*(sizeof(task_t)+max_body_size));
  }

  tc->qtype = qtype;

  tc->clod = clod_create(GTC_MAX_CLOD_CLOS);

  tc->td = td_create();

  tc->max_body_size = max_body_size;
  tc->terminated    = 0;

  gtc_ldbal_cfg_set(gtc, ldbal_cfg);

  switch (tc->qtype) {
    case GtcQueueSDC:
      gtc_create_sdc(gtc, max_body_size, shrb_size, ldbal_cfg);
      break;
    default:
      gtc_eprintf(DBGERR, "gtc_create: unsupported queue type\n");
      exit(1);
      break;
  }

  return gtc;
}



/**
 * gtc_destroy-- frees a new dht
 * @param dht - the dht to free
 */
void gtc_destroy(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  tc->cb.destroy(gtc);

  td_destroy(tc->td);
  clod_destroy(tc->clod);
  if (tc->steal_buf)
    free(tc->steal_buf);
  if (tc->timers)
    free(tc->timers);
  if (tc->tsctimers) 
    free(tc->tsctimers);

  free(tc);

  gtc_handle_release(gtc);

  if ((_c->total_tcs == 0) && (_c->auto_teardown == 1))
    gtc_fini();
}



/**
 * Reset a task collection so it can be reused and removes any remaining tasks.
 * Collective call.
 *
 * @param tc       IN Pointer to task collection
 */
void gtc_reset(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  // Reset to inactive state
  tc->state = STATE_INACTIVE;

  // Reset stats
  tc->tasks_completed = 0;
  tc->tasks_spawned   = 0;
  tc->tasks_stolen    = 0;
  tc->num_steals      = 0;
  tc->passive_count   = 0;
  tc->failed_steals_locked   = 0;
  tc->failed_steals_unlocked = 0;
  tc->aborted_steals         = 0;
  tc->aborted_victims        = 0;
  tc->dispersion_attempts_unlocked = 0;
  tc->dispersion_attempts_locked   = 0;

  // Reset round-robin counter
  tc->last_victim = (_c->rank + 1) % _c->size;

  // Zero out the timers
  memset(tc->timers, 0, sizeof(tc_timers_t));

  // Reset termination detection
  tc->dispersed  = 0;
  tc->terminated = 0;
  tc->external_work_avail = 0;

  td_reset(tc->td);

  tc->cb.reset(gtc);
}



/**
 * String that gives the name of the current queue
 */
char *gtc_queue_name(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->cb.queue_name();
}



/**
 * Print the configuration of the given task colleciton. Non-collective.
 *
 * @param gtc Portal reference to desired TC.
 */
void gtc_print_config(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  int idx = 0, size = 1000;
  char *msg = alloca(size*sizeof(char));

  idx += snprintf(msg+idx, size-idx, "Queue: %s", gtc_queue_name(gtc));

  idx += snprintf(msg+idx, size-idx, ", Mutexes: %s", "PtlSwap Spinlocks");

  if (tc->ldbal_cfg.stealing_enabled) {
    idx += snprintf(msg+idx, size-idx, ", Victim selection: %s", victim_methods[tc->ldbal_cfg.victim_selection]);

    if (tc->ldbal_cfg.steal_method == STEAL_CHUNK)
      idx += snprintf(msg+idx, size-idx, ", Steal method: %s (%d)", steal_methods[tc->ldbal_cfg.steal_method], tc->ldbal_cfg.chunk_size);
    else
      idx += snprintf(msg+idx, size-idx, ", Steal method: %s", steal_methods[tc->ldbal_cfg.steal_method]);

    if (tc->ldbal_cfg.local_search_factor > 0)
      idx += snprintf(msg+idx, size-idx, ", Locality-aware stealing");

    if (tc->ldbal_cfg.steals_can_abort)
      idx += snprintf(msg+idx, size-idx, ", Aborting Steals");

  } else {
    idx += snprintf(msg+idx, size-idx, ", Stealing disabled");
  }

  printf("Task collection %d -- %s\n", gtc, msg);
}


/**
 * Add task to the task collection.  Task is copied in and task buffer is available
 * to the user when call returns.  Non-collective call.
 *
 * @param tc       IN Ptr to task collection
 * @param proc     IN Process # whose task collection this task is to be added
 *                    to. Common case is tc->procid
 * @param task  INOUT Task to be added. user manages buffer when call
 *                    returns. This call fills in task field and the contents 
 *                    of task will match what is in the queue
 *                    when the call returns.
 * @return 0 on success.
 */
int gtc_add(gtc_t gtc, task_t *task, int proc) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->cb.add(gtc, task, proc);
}



/**
 * Create-and-add a task in-place on the head of the queue.  Note, you should
 * not do *ANY* other queue operations until all outstanding in-place creations
 * have finished.  The pointer returned points directly to an element in the
 * queue.  Do not add it, do not free it, discard the pointer when you are finished
 * assigning the task body.
 *
 * @param gtc    Portable reference to the task collection
 * @param tclass Desired task class
 */
task_t *gtc_task_inplace_create_and_add(gtc_t gtc, task_class_t tclass) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->cb.inplace_create_and_add(gtc, tclass);
}


/**
 * Complete an in-place task creation.  Note, you should not do *ANY* other
 * queue operations until all outstanding in-place creations have finished.
 *
 * @param gtc    Portable reference to the task collection
 * @param task   The pointer that was returned by inplace_create_and_add()
 */
void gtc_task_inplace_create_and_add_finish(gtc_t gtc, task_t *t) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->cb.inplace_ca_finish(gtc, t);
}



/** Invoke the progress engine.  Update work queues, balance the schedule,
 *  make progress on communication.
 */
void gtc_progress(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  tc->cb.progress(gtc);
}



/**
 * Number of tasks available in local task collection.  Note, this is an
 * approximate number since we're not locking the data structures.
 */
int gtc_tasks_avail(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->cb.tasks_avail(gtc);
}



/**
 * Turn on stealing for this task collection.  Stealing is enabled by default.
 * Non-collective call, use a barrier if you'd like this to be collective.
 *
 * @param tc       IN Pointer to task collection.
 */
void gtc_enable_stealing(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  assert(!tc->ldbal_cfg.stealing_enabled);
  tc->ldbal_cfg.stealing_enabled = 1;
}


/**
 * Turn off stealing for this task collection.  Stealing is enabled by default.
 * Non-collective call, use a barrier if you'd like this to be collective.
 *
 * @param tc       IN Pointer to task collection.
 */
void gtc_disable_stealing(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  assert(tc->ldbal_cfg.stealing_enabled);
  tc->ldbal_cfg.stealing_enabled = 0;
}


/**
 * Change the state of the external work flag.  Note, this flag should be stable.
 * Once it transition from true to false it should stay that way in order to
 * ensure proper termination detection.
 *
 * @param tc   IN Portable reference to the task collection
 * @param flag IN flag == 0 -> No work, flag > 0 -> External work
 */
void gtc_set_external_work_avail(gtc_t gtc, int flag) {
  tc_t *tc = gtc_lookup(gtc);
  tc->external_work_avail = flag;
}


/**
 * Get a task from the head of the local patch of the task collection.  This
 * function DOES NOT invoke load balancing, it only checks the local queue for
 * work.  It returns NULL when no local work is available, so a NULL does NOT
 * imply that global termination has been detected.
 *
 * @param gtc      IN Ptr to task collection
 * @return         Ptr to task (from local queue or stolen). NULL if none
 *                 found.  A NULL result here means that global termination
 *                 has occurred.  Returned buffer should be deleted by the user.
 */
int gtc_get_local_buf(gtc_t gtc, int priority, task_t *buf) {
  tc_t *tc = gtc_lookup(gtc);

  return tc->cb.pop_head(tc->shared_rb, _c->rank, buf);
}

/**
 * gtc_steal_tail -- Attempt to steal a chunk of tasks from the given victim's tail.
 *
 * @param  victim        Process ID of victim
 * @param  req_stealsize Amount of work to steal.
 * @return number of tasks stolen
 */
int gtc_steal_tail(gtc_t gtc, int victim) {
  tc_t *tc = gtc_lookup(gtc);
  int   stealsize;
  int   req_stealsize;
  tc_tsctimer_t temp;

  if (tc->ldbal_cfg.steal_method == STEAL_CHUNK)
    req_stealsize = tc->ldbal_cfg.chunk_size;
  else
    req_stealsize = __GTC_MAX_STEAL_SIZE;

  // TSC timer code is ugly because of inflexible macros
  temp.total = 0;
  temp.last = gtc_get_tsctime();
  stealsize = tc->cb.pop_n_tail(tc->shared_rb, victim, req_stealsize, tc->steal_buf, tc->ldbal_cfg.steal_method);
  temp.temp = gtc_get_tsctime();

  // account into success or failed steal timers
  if (stealsize > 0)
    tc->tsctimers->getsteal.total += (temp.temp - temp.last);
  else
    tc->tsctimers->getfail.total  += (temp.temp - temp.last);

  if (stealsize > 0) {
    gtc_lprintf(DBGGET, "\tthread %d: steal try: %d got: %d tasks from thread %d\n", _c->rank, req_stealsize, stealsize, victim);
    tc->cb.push_n_head(tc->shared_rb, _c->rank, tc->steal_buf, stealsize);
  } else if (stealsize < 0) {
    //gtc_lprintf(DBGGET, "\tthread %d: Aborting steal from %d\n", _c->rank, victim);
    // XXX should account for number of aborted steals?
  } else {
    //gtc_lprintf(DBGGET, "\tthread %d: failed steal got no tasks from thread %d\n", _c->rank, victim);
  }

  return stealsize;
}


/**
 * gtc_try_steal_tail -- Attempt to steal a chunk of tasks from the given victim's tail.
 *
 * @param  victim        Process ID of victim
 * @param  req_stealsize Amount of work to steal.  If this number is negative, perform work
 *                       splitting and get up to abs(req_stealsize) tasks.
 * @return number of tasks stolen or -1 on failure
 */
int gtc_try_steal_tail(gtc_t gtc, int victim) {
  tc_t *tc = gtc_lookup(gtc);
  int   stealsize;
  int   req_stealsize;

  if (tc->ldbal_cfg.steal_method == STEAL_CHUNK)
    req_stealsize = tc->ldbal_cfg.chunk_size;
  else
    req_stealsize = __GTC_MAX_STEAL_SIZE;

  gtc_lprintf(DBGGET, "  thread %d: attempting to steal %d tasks from thread %d\n", _c->rank, req_stealsize, victim);

#ifdef QUEUE_TRY_POP_N_TAIL
  stealsize = tc->cb.try_pop_n_tail(tc->shared_rb, victim, req_stealsize, tc->steal_buf, tc->ldbal_cfg.steal_method);
#else
  stealsize = tc->cb.pop_n_tail(tc->shared_rb, victim, req_stealsize, tc->steal_buf, tc->ldbal_cfg.steal_method);
#endif

  if (stealsize > 0) {
    gtc_lprintf(DBGGET, "  thread %d: Got %d tasks, pushing onto my head\n", _c->rank, stealsize);
    tc->cb.push_n_head(tc->shared_rb, _c->rank, tc->steal_buf, stealsize);
  } else if (stealsize < 0) {
    gtc_lprintf(DBGGET, "  thread %d: Aborting steal from %d\n", _c->rank, victim);
  }

  gtc_lprintf(DBGGET, "  thread %d: steal completed\n", _c->rank);

  return stealsize;
}



/** Internal victim selector state machine: Select the next victim to attempt a steal from.
  *
  * @param[in] gtc   Current task collection
  * @param[in] state State of the victim selector.  The state struct should be
  *                  initially set to 0
  * @return          Next victim
  */
int gtc_select_victim(gtc_t gtc, gtc_vs_state_t *state) {
  int v;
  tc_t *tc;

  tc = gtc_lookup(gtc);
  v  = -1;

  /* SINGLE: Single processor run
   */
  if (_c->size == 1) {
    v = 0;
  }

  /* RETRY: Attempt to steal from the same victim again.  This is used
   * with aborting steals which are non-blocking a require retrying.
   */
  if (state->victim_retry) {
    // Note: max_steal_retries < 0 means infinite number of retries
    if (state->num_retries >= tc->ldbal_cfg.max_steal_retries && tc->ldbal_cfg.max_steal_retries > 0) {
      state->num_retries = 0;
      tc->aborted_victims++;

    } else {
      state->victim_retry = 0;
      state->num_retries++;
      v = state->last_victim;
    }
  }

  /* FREE: Free victim selection.
   */
  if (v < 0) {
    // Victim Random: Randomly select the next victim
    if (tc->ldbal_cfg.victim_selection == VICTIM_RANDOM) {
      do {
        v = rand() % _c->size;
      } while (v == _c->rank);
    }

    // Round Robin: Next victim is selected round-robin
    else if (tc->ldbal_cfg.victim_selection == VICTIM_ROUND_ROBIN) {
      v = (state->last_victim + 1) % _c->size;
    }

    else {
      printf("Unknown victim selection method\n");
      assert(0);
    }
  }

  state->last_victim = v;

  return v;
}



/**
 * Processes the task collection. Collective call. Handles load-balancing
 * and stealing if it is enabled. Returns collectively.
 *
 * @param tc       IN Ptr to task collection
 */
void gtc_process(gtc_t gtc) {
  tc_t   *tc   = gtc_lookup(gtc);

  // Struct to transmit termination flag and task descriptor to workers
  struct xmit_task_s {
    int    terminated;
    task_t task;
  };

  struct xmit_task_s *xtask;
  const int           xtask_size = sizeof(struct xmit_task_s) + tc->max_body_size;

  xtask = malloc(xtask_size);
  xtask->terminated = 0;

  // default we are just grabbing a task and running with it:
  // we are the exec group
  // we are in the steal group

  gtc_lprintf(DBGGROUP, "  Processing multilevel parallel TC - master id %4d\n", _c->rank);

#ifdef GTC_TRACE
  char buf[100];
  FILE *trace_file;
  sprintf(buf, "/scratch/scioto-trace-%d.log", _c->rank);
  trace_file = fopen(buf, "w");
  gtc_trace_start(gtc, 10000, -1, trace_file);
#endif

  shmem_barrier_all();

  TC_START_TIMER(tc, process);
  tc->state = STATE_SEARCHING;

  while (tc->cb.get_buf(gtc, 0, &xtask->task)) {
    // Run the task we just got
    gtc_task_execute(gtc, &xtask->task);
  }

  free(xtask);
  tc->state = STATE_TERMINATED;
  TC_STOP_TIMER(tc, process);

#ifdef GTC_TRACE
  shmem_barrier_all();
  gtc_trace_stop();
  fclose(trace_file);
#endif

#if 0
  if (getenv("SCIOTO_CHECK_PROCESSED_EQUAL_SPAWNED")) {
    unsigned long processed, spawned;
    unsigned long grp_processed = 0;

    grp_processed = tc->tasks_completed;

    gtc_reduce(&grp_processed, &processed, GtcReduceOpSum, LongType, 1);
    gtc_reduce(&tc->tasks_spawned, &spawned, GtcReduceOpSum, LongType, 1);
    //MPI_Reduce(&grp_processed, &processed, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, tc->comm);
    //MPI_Reduce(&tc->tasks_spawned, &spawned, 1, MPI_UNSIGNED_LONG, MPI_SUM, 0, tc->comm);

    if (_c->rank == 0) {
      printf("GTC internal check: %lu tasks spawned, %lu tasks processed, %s.\n", spawned, processed,
          (processed == spawned) ? "PASS" : "FAIL");
    }
  }
#endif /* not used */

  assert(gtc_tasks_avail(gtc) == 0);
}



/**
 * Print stats for this task collection.
 * @param tc       IN Ptr to task collection
 */
void gtc_print_gstats(gtc_t gtc) {
  char *stats_disabled         = getenv("SCIOTO_DISABLE_STATS");
  char *pernode_stats_disabled = getenv("SCIOTO_DISABLE_PERNODE_STATS");
  char *ext_stats_enabled      = getenv("SCIOTO_EXTENDED_STATS");
  char *unordered_stats         = getenv("SCIOTO_UNORDERED_STATS");
  unsigned long tStolen, tSteals;
  double  dispersion, maxDispersion, acquire, imbalance, maxImbalance, minImbalance;
  double  search, tSearch, passive, tPassive, tAcquire;
  tc_t *tc = gtc_lookup(gtc);

  if (stats_disabled) return;

  shmem_barrier_all();
  if (!pernode_stats_disabled) {
    if (unordered_stats) {
      gtc_print_my_stats(gtc);
    } else {
      for (int i=0; i< _c->size; i++) {
        if (i == _c->rank)
          gtc_print_my_stats(gtc);
        shmem_barrier_all();
      }
    }
  }


  if (ext_stats_enabled == NULL) {

    fflush(NULL);
    shmem_barrier_all();

    dispersion = TC_READ_TIMER_SEC(tc, dispersion);
    imbalance  = TC_READ_TIMER_SEC(tc, imbalance);
    acquire    = tc->num_steals ?  (TC_READ_TIMER_SEC(tc, passive) - TC_READ_TIMER_SEC(tc, imbalance))/(tc->num_steals) : 0.0;
    search     = TC_READ_TIMER_SEC(tc, search);
    passive    = TC_READ_TIMER_SEC(tc, passive);

    // global stats
    gtc_reduce(&tc->tasks_stolen, &tStolen, GtcReduceOpSum, LongType, 1);
    gtc_reduce(&tc->num_steals,   &tSteals, GtcReduceOpSum, LongType, 1);

    acquire    = (tc->num_steals) ?  (TC_READ_TIMER_SEC(tc, passive) - TC_READ_TIMER_SEC(tc, imbalance))/(tc->num_steals) : 0.0;

    gtc_reduce(&dispersion, &maxDispersion, GtcReduceOpMax, DoubleType, 1);
    gtc_reduce(&imbalance,  &maxImbalance,  GtcReduceOpMax, DoubleType, 1);
    gtc_reduce(&imbalance,  &minImbalance,  GtcReduceOpMin, DoubleType, 1);
    gtc_reduce(&acquire,    &tAcquire,      GtcReduceOpSum, DoubleType, 1);
    gtc_reduce(&search,     &tSearch,       GtcReduceOpSum, DoubleType, 1);
    gtc_reduce(&passive,    &tPassive,      GtcReduceOpSum, DoubleType, 1);

    shmem_barrier_all();

    eprintf("Total  : stolen %3lu, steals %3lu Average: stolen %3lu, steals %3lu\n", tStolen, tSteals, (tStolen)/_c->size, (tSteals)/_c->size);
    eprintf("Time   : worst dispersion %8.5fms, worst imbalance %8.5fms, best imbalance %8.5fms, avg acquire %8.5fms, avg search %8.5fs (%5.2f %%)\n",
          maxDispersion*1000.0, maxImbalance*1000.0, minImbalance*1000.0, tAcquire/_c->size*1000.0, tSearch/_c->size, tSearch/tPassive);
    tc->cb.print_gstats(gtc);
  }
  fflush(NULL);
  shmem_barrier_all();

  double passive_loc, passive_rem;
  double process_loc, process_rem;
  unsigned long tCompleted;

  process_loc = TC_READ_TIMER_SEC(tc, process);
  passive_loc = TC_READ_TIMER_SEC(tc, passive);

  gtc_reduce(&tc->tasks_completed, &tCompleted, GtcReduceOpSum, UnsignedLongType, 1);

  gtc_reduce(&process_loc, &process_rem, GtcReduceOpSum, DoubleType, 1);
  gtc_reduce(&passive_loc, &passive_rem, GtcReduceOpSum, DoubleType, 1);
  shmem_barrier_all();

  struct timespec sleep = { 0, 25 * 1000000L }; // 25ms
  nanosleep(&sleep, NULL);       // lag a little to make sure that final results appear after all other stats i/o is done.

  eprintf("SCIOTO : Process time %.5f s, passive time %.5f s (%.2f%%), %lu tasks completed, %.2f tasks/sec (%.2f tasks/sec/PE)\n",
      process_rem/_c->size, passive_rem/_c->size, passive_rem/process_rem*100.0, tCompleted,
      tCompleted/(process_rem/_c->size), tCompleted/process_rem);

  fflush(NULL);
  shmem_barrier_all();
}




void gtc_print_stats(gtc_t gtc) {
  char buf[100], buf2[100];
  double passive_loc, passive_rem;
  double process_loc, process_rem;
  double search_loc, search_rem;
  unsigned long tCompleted;
  char *stats_disabled         = getenv("SCIOTO_DISABLE_STATS");
  char *pernode_stats_disabled = getenv("SCIOTO_DISABLE_PERNODE_STATS");
  char *unordered_stats         = getenv("SCIOTO_UNORDERED_STATS");
  tc_t *tc = gtc_lookup(gtc);

  if (stats_disabled) return;

  shmem_barrier_all();
  if (!pernode_stats_disabled) {
    if (unordered_stats) {
      gtc_print_my_stats(gtc);
    } else {
      for (int i=0; i< _c->size; i++) {
        if (i == _c->rank)
          gtc_print_my_stats(gtc);
        shmem_barrier_all();
      }
    }
  }
  fflush(NULL);
  shmem_barrier_all();


  process_loc = TC_READ_TIMER_SEC(tc, process);
  passive_loc = TC_READ_TIMER_SEC(tc, passive);
  search_loc  = TC_READ_TIMER_MSEC(tc, search);

  gtc_reduce(&tc->tasks_completed, &tCompleted,  GtcReduceOpSum, UnsignedLongType, 1);
  gtc_reduce(&process_loc,         &process_rem, GtcReduceOpSum, DoubleType, 1);
  gtc_reduce(&passive_loc,         &passive_rem, GtcReduceOpSum, DoubleType, 1);
  gtc_reduce(&search_loc,          &search_rem,  GtcReduceOpSum, DoubleType, 1);
  shmem_barrier_all();
  
  // sanity check timing

  TC_INIT_TIMER(tc, t[0]);
  TC_INIT_TSCTIMER(tc, sanity);
  struct timespec sleep = { 0, 25 * 1000000L }; // 25ms
  TC_START_TIMER(tc, t[0]);
  TC_START_TSCTIMER(tc, sanity);
  nanosleep(&sleep, NULL);       // lag a little to make sure that final results appear after all other stats i/o is done.
  TC_STOP_TIMER(tc, t[0]);
  TC_STOP_TSCTIMER(tc, sanity);

  // 9 char to colon
  eprintf("SCIOTWO : queue: %s \n", gtc_queue_name(gtc));
  eprintf("        : process time %.5f s, passive time %.5f s (%.2f%%), search time %.5f ms\n",
                     process_rem/_c->size, passive_rem/_c->size, passive_rem/process_rem*100.0,
                     search_rem/_c->size);
  eprintf("        : tasks completed %lu, %.2f tasks/sec (%.2f tasks/sec/PE) timer sanity: clocktime: %.2f ms TSC %.2f ms\n",
                     tCompleted, tCompleted/(process_rem/_c->size), tCompleted/process_rem,
                     TC_READ_TIMER_MSEC(tc, t[0]), TC_READ_TSCTIMER_MSEC(tc,sanity));

  eprintf("        : dispersion %s attempts %32s\n", 
      gtc_print_mmad(buf, "ms", TC_READ_TIMER_MSEC(tc, dispersion), 0),
      gtc_print_mmad(buf2, "", tc->dispersion_attempts_locked+tc->dispersion_attempts_unlocked, 1));
  eprintf("        : imbalance  %s\n", gtc_print_mmad(buf, "ms", TC_READ_TIMER_MSEC(tc, imbalance), 0));

  tc->cb.print_gstats(gtc);

  double lDisp, lAtt, tDisp, tAtt;
  lDisp = TC_READ_TIMER_MSEC(tc, dispersion);
  lAtt  = tc->dispersion_attempts_locked + tc->dispersion_attempts_unlocked;
  gtc_reduce(&lDisp, &tDisp, GtcReduceOpSum, DoubleType, 1);
  gtc_reduce(&lAtt,  &tAtt,  GtcReduceOpSum, DoubleType, 1);
  shmem_barrier_all();

  eprintf("%lu      %.5f %lu %.2f %.2f %.2f\n", _c->size, process_rem/_c->size, tCompleted, tCompleted/(process_rem/_c->size), tDisp/_c->size, tAtt/_c->size);

  fflush(NULL);
  shmem_barrier_all();

}




void gtc_print_my_stats(gtc_t gtc) {
  double        avg_acquire_time_ms;
  tc_t         *tc = gtc_lookup(gtc);

  char *stats_disabled         = getenv("SCIOTO_DISABLE_STATS");
  char *pernode_stats_disabled = getenv("SCIOTO_DISABLE_PERNODE_STATS");

  if (stats_disabled) return;

  avg_acquire_time_ms = (TC_READ_TIMER(tc, passive) - TC_READ_TIMER(tc, imbalance))/(tc->num_steals * (double)10e6);

  if (!pernode_stats_disabled) {
    printf(" %4d - Tasks: completed %3lu, spawned %3lu, stolen %3lu\n"
           " %4d -      : nsteals %3lu, steal fails -- %3lu, aborted %3lu\n"
           " %4d -  Time: process %8.5fs = active %8.5fs + passive %8.5fs, passive count=%lu (%8.5fms avg), searching %8.5fs (%5.2f %%)\n"
           " %4d -      : dispersion %8.5fms, disp. steal fails -- unlocked %3lu, locked %3lu\n"
           " %4d -      : avg acquire %8.5fms, imbalance %8.5fms\n"
           " %4d -      : timers: %8.5fms %8.5fms %8.5fms %8.5f %8.5fms\n",
                _c->rank,
                  tc->tasks_completed, tc->tasks_spawned, tc->tasks_stolen,
                _c->rank,
                  tc->num_steals, tc->failed_steals_unlocked + tc->failed_steals_locked, tc->aborted_steals,
                _c->rank,
                  TC_READ_TIMER_SEC(tc, process),
                  TC_READ_TIMER_SEC(tc, process) - TC_READ_TIMER_SEC(tc, passive),
                  TC_READ_TIMER_SEC(tc, passive),
                  tc->passive_count,
                  TC_READ_TIMER_MSEC(tc, passive) / tc->passive_count,
                  TC_READ_TIMER_SEC(tc, search),
                  (TC_READ_TIMER(tc, search)/(double)TC_READ_TIMER(tc, process))*100.0,
                _c->rank,
                  TC_READ_TIMER_MSEC(tc, dispersion),
                  tc->dispersion_attempts_unlocked,
                  tc->dispersion_attempts_locked,
                _c->rank,
                  (tc->num_steals > 0) ?  avg_acquire_time_ms : 0.6, TC_READ_TIMER_MSEC(tc, imbalance),
                _c->rank,
                  TC_READ_TIMER_MSEC(tc, t[0]),
                  TC_READ_TIMER_MSEC(tc, t[1]),
                  TC_READ_TIMER_MSEC(tc, t[2]),
                  TC_READ_TIMER_MSEC(tc, t[3]),
                  TC_READ_TIMER_MSEC(tc, t[4])
        );
    tc->cb.print_stats(gtc);
  }
}



/** Query the number of tasks that have been executed by the local process.
  */
unsigned long gtc_stats_tasks_completed(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->tasks_completed;
}


/** Query the number of tasks that the local process has spawned.
  */
unsigned long gtc_stats_tasks_spawned(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  return tc->tasks_spawned;
}

