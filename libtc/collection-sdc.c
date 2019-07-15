/*
 * Copyright (C) 2018. See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <alloca.h>
#include <math.h>

#include "tc.h"

#include "sdc_shr_ring.h"
//#include "shr_ring.h"

/**
 * Create a new task collection.  Collective call.
 *
 * @param[in] max_body_size Max size of a task descriptor's body in bytes for this tc.
 *                         Any task that is added must be smaller or equal to this size.
 * @param[in] shrb_size    Size of the local task queue (in tasks).
 * @param[in] cfg          Load balancer configuation.  NULL for default configuration.
 *
 * @return                 Portable task collection handle.
 */
gtc_t gtc_create_sdc(gtc_t gtc, int max_body_size, int shrb_size, gtc_ldbal_cfg_t *cfg) {
  tc_t  *tc;

  tc  = gtc_lookup(gtc);

  // Allocate the shared ring buffer.  Total task size is the size
  // of the header + max_body size.
  tc->shared_rb = sdc_shrb_create(tc->max_body_size + sizeof(task_t), shrb_size);
  //tc->inbox = shrb_create(tc->max_body_size + sizeof(task_t), shrb_size);
  tc->inbox = NULL;

  tc->cb.destroy                = gtc_destroy_sdc;
  tc->cb.reset                  = gtc_reset_sdc;
  tc->cb.get_buf                = gtc_get_buf_sdc;
  tc->cb.add                    = gtc_add_sdc;
  tc->cb.inplace_create_and_add = gtc_task_inplace_create_and_add_sdc;
  tc->cb.inplace_ca_finish      = gtc_task_inplace_create_and_add_finish_sdc;
  tc->cb.progress               = gtc_progress_sdc;
  tc->cb.tasks_avail            = gtc_tasks_avail_sdc;
  tc->cb.queue_name             = gtc_queue_name_sdc;
  tc->cb.print_stats            = gtc_print_stats_sdc;
  tc->cb.print_gstats           = gtc_print_gstats_sdc;

  tc->cb.pop_head               = sdc_shrb_pop_head;
  tc->cb.pop_n_tail             = sdc_shrb_pop_n_tail;
  tc->cb.try_pop_n_tail         = sdc_shrb_try_pop_n_tail;
  tc->cb.push_n_head            = sdc_shrb_push_n_head;
  tc->cb.work_avail             = sdc_shrb_size;

  tc->qsize = sizeof(sdc_shrb_t);

  shmem_barrier_all();

  return gtc;
}



/**
 * Destroy task collection.  Collective call.
 */
void gtc_destroy_sdc(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  sdc_shrb_destroy(tc->shared_rb);
  //shrb_destroy(tc->inbox);
}



/**
 * Reset a task collection so it can be reused and removes any remaining tasks.
 * Collective call.
 */
void gtc_reset_sdc(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  sdc_shrb_reset(tc->shared_rb);
  //shrb_reset(tc->inbox);
}



/**
 * String that gives the name of this queue
 */
char *gtc_queue_name_sdc() {
#ifdef SDC_NODC
  return "Split (NODC)";
#else
  return "Split Deferred-Copy";
#endif
}



/** Invoke the progress engine.  Update work queues, balance the schedule,
 *  make progress on communication.
 */
void gtc_progress_sdc(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  //TC_START_TIMER(tc, t[0]);
  TC_START_TSCTIMER(tc,progress);

#if 0 /* no task pushing */
  // Check the inbox for new work
  if (shrb_size(tc->inbox) > 0) {
    int   ntasks, npopped;
    void *work;

    ntasks = 100;
    work   = malloc((tc->max_body_size+sizeof(task_t))*ntasks);
    npopped= shrb_pop_n_tail(tc->inbox, _c->rank, ntasks, work, STEAL_CHUNK);

    sdc_shrb_push_n_head(tc->shared_rb, _c->rank, work, npopped);
    shrb_free(work);

    gtc_lprintf(DBGINBOX, "gtc_progress: Moved %d tasks from inbox to my queue\n", npopped);
  }
#endif /* no task pushing */

  // Update the split
  sdc_shrb_release(tc->shared_rb);

  // Attempt to reclaim space
  sdc_shrb_reclaim_space(tc->shared_rb);
  tc->shared_rb->nprogress++;
  //TC_STOP_TIMER(tc,t[0]);
  TC_STOP_TSCTIMER(tc,progress);
}



/**
 * Number of tasks available in local task collection.  Note, this is an
 * approximate number since we're not locking the data structures.
 */
int gtc_tasks_avail_sdc(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  //return sdc_shrb_size(tc->shared_rb) + shrb_size(tc->inbox);
  return sdc_shrb_size(tc->shared_rb);
}


/**
 * Find work to do, search everywhere. Use when you write your own
 * gtc_process() implementation.  Do NOT use together with
 * gtc_process().  This function invokes load balancing to attempt to locate
 * work when none is available locally.  It returns NULL only when global
 * termination has been detected.
 *
 * @param tc       IN Ptr to task collection
 * @return         Ptr to task (from local queue or stolen). NULL if none
 *                 found.  A NULL result here means that global termination
 *                 has occurred.  Returned buffer should be deleted by the user.
 */
double gtc_get_dummy_work = 0.0;
int gtc_get_buf_sdc(gtc_t gtc, int priority, task_t *buf) {
  tc_t   *tc = gtc_lookup(gtc);
  int     got_task = 0;
  int     v, steal_size;
  int     passive = 0;
  int     searching = 0;
  gtc_vs_state_t vs_state = {0};
  void *rb_buf;

  tc->getcalls++;
  TC_START_TSCTIMER(tc,getbuf);

  // Invoke the progress engine
  gtc_progress(gtc);

  // Try to take my own work first.  We take from the head of our own queue.
  // When we steal, we take work off of the tail of the victim's queue.
  got_task = gtc_get_local_buf(gtc, priority, buf);

  // Time dispersion.  If I had work to start this should be ~0.
  if (!tc->dispersed) TC_START_TIMER(tc, dispersion);

  // No more local work, try to steal some
  if (!got_task && tc->ldbal_cfg.stealing_enabled) {
    gtc_lprintf(DBGGET, " Thread %d: gtc_get() searching for work\n", _c->rank);

#ifndef NO_SEATBELTS
    TC_START_TIMER(tc, passive);
    TC_INIT_TIMER(tc, imbalance);
    TC_START_TIMER(tc, imbalance);
    passive = 1;
    tc->passive_count++;
#endif

    rb_buf = malloc(tc->qsize);
    assert(rb_buf != NULL);

    vs_state.last_victim = tc->last_victim;

    // Keep searching until we find work or detect termination
    while (!got_task && !tc->terminated) {
      int      max_steal_attempts, steal_attempts, steal_done;
      void *victim_rb;

      tc->state = STATE_SEARCHING;

      if (!searching) {
        TC_START_TIMER(tc, search);
        searching = 1;
      }

      // Select the next victim
      v = gtc_select_victim(gtc, &vs_state);

      max_steal_attempts = tc->ldbal_cfg.max_steal_attempts_remote;
      victim_rb = rb_buf;

      TC_START_TSCTIMER(tc,poptail); // this counts as attempting to steal
      // sdc_shrb_fetch_remote_trb(tc->shared_rb, victim_rb, v);
      shmem_getmem(victim_rb, tc->shared_rb, sizeof(sdc_shrb_t), v);
      TC_STOP_TSCTIMER(tc,poptail);

      // Poll the victim for work.  In between polls, maintain progress on termination detection.
      for (steal_attempts = 0, steal_done = 0;
           !steal_done && !tc->terminated && steal_attempts < max_steal_attempts;
           steal_attempts++) {

        // Apply linear backoff to avoid flooding remote nodes
        if (steal_attempts > 0) {
          int j;
          for (j = 0; j < steal_attempts*1000; j++)
            gtc_get_dummy_work += 1.0;
        }

        if (tc->cb.work_avail(victim_rb) > 0) {
          tc->state = STATE_STEALING;

          if (searching) {
#ifndef NO_SEATBELTS
            TC_STOP_TIMER(tc, search);
#endif
            searching = 0;
          }

          // Perform a steal/try_steal
          if (tc->ldbal_cfg.steals_can_abort)
            steal_size = gtc_try_steal_tail(gtc, v);
          else
            steal_size = gtc_steal_tail(gtc, v);

          // Steal succeeded: Got some work from remote node
          if (steal_size > 0) {
            tc->tasks_stolen += steal_size;
            tc->num_steals += 1;
            steal_done = 1;
            tc->last_victim = v;

          // Steal failed: Got the lock, no longer any work on remote node
          } else if (steal_size == 0) {
            tc->failed_steals_locked++;
            steal_done = 1;

          // Steal aborted: Didn't get the lock, refresh victim metadata and try again
          } else {
            if (steal_attempts + 1 == max_steal_attempts)
              tc->aborted_steals++;
            vs_state.victim_retry = 1;
          }

        } else /* ! (QUEUE_WORK_AVAIL(victim_rb) > 0) */ {
          tc->failed_steals_unlocked++;
          steal_done = 1;
        }

        // Invoke the progress engine
        gtc_progress(gtc);

        // Still no work? Lock to be sure and check for termination.
        // Locking is only needed here if we allow pushing.
        // TODO: New TD should not require locking.  Remove locks and test.
        if (gtc_tasks_avail(gtc) == 0 && !tc->external_work_avail) {
          //QUEUE_LOCK(tc->shared_rb, _c->rank);
          //shrb_lock(tc->inbox, _c->rank); /* no task pushing */

          if (gtc_tasks_avail(gtc) == 0 && !tc->external_work_avail) {
            td_set_counters(tc->td, tc->tasks_spawned, tc->tasks_completed);
            tc->terminated = td_attempt_vote(tc->td);
          }

          //shrb_unlock(tc->inbox, _c->rank); /* no task pushing */
          //QUEUE_UNLOCK(tc->shared_rb, _c->rank);

        // We have work, done stealing
        } else {
          steal_done = 1;
        }
      }

      if (gtc_tasks_avail(gtc))
        got_task = gtc_get_local_buf(gtc, priority, buf);
    }

    free(rb_buf);
  } else {
    tc->getlocal++;
  }

#ifndef NO_SEATBELTS
  if (passive) TC_STOP_TIMER(tc, passive);
  if (passive) TC_STOP_TIMER(tc, imbalance);
  if (searching) TC_STOP_TIMER(tc, search);
#endif

  // Record how many attempts it took for our first get, this is the number of
  // attempts during the work dispersion phase.
  if (!tc->dispersed) {
    if (passive) TC_STOP_TIMER(tc, dispersion);
    tc->dispersed = 1;
    tc->dispersion_attempts_unlocked = tc->failed_steals_unlocked;
    tc->dispersion_attempts_locked   = tc->failed_steals_locked;
  }

  gtc_lprintf(DBGGET, " Thread %d: gtc_get() %s\n", _c->rank, got_task? "got work":"no work");
  if (got_task) tc->state = STATE_WORKING;
  TC_STOP_TSCTIMER(tc,getbuf);
  return got_task;
}



/**
 * Add task to the task collection.  Task is copied in and task buffer is available
 * to the user when call returns.  Non-collective call.
 *
 * @param tc       IN Ptr to task collection
 * @param proc     IN Process # whose task collection this task is to be added
 *                    to. Common case is tc->procid
 * @param task  INOUT Task to be added. user manages buffer when call
 *                    returns. Preferably allocated in ARMCI local allocated memory when
 *                    proc != tc->procid for improved RDMA performance.  This call fills
 *                    in task field and the contents of task will match what is in the queue
 *                    when the call returns.
 *
 * @return 0 on success.
 */
int gtc_add_sdc(gtc_t gtc, task_t *task, int proc) {
  tc_t *tc = gtc_lookup(gtc);

  assert(gtc_task_body_size(task) <= tc->max_body_size);
  assert(tc->state != STATE_TERMINATED);
  TC_START_TSCTIMER(tc,add);

  task->created_by = _c->rank;

  if (proc == _c->rank) {
    // Local add: put it straight onto the local work list
    sdc_shrb_push_head(tc->shared_rb, _c->rank, task, sizeof(task_t) + gtc_task_body_size(task));
  } 
#if 0 /* no task pushing */
  else {
    // Remote adds: put this in the remote node's inbox
    if (task->affinity == 0)
      shrb_push_head(tc->inbox, proc, task, sizeof(task_t) + gtc_task_body_size(task));
    else
      shrb_push_tail(tc->inbox, proc, task, sizeof(task_t) + gtc_task_body_size(task));
  }
#endif /* no task pushing */

  ++tc->tasks_spawned;
  TC_STOP_TSCTIMER(tc,add);

  return 0;
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
task_t *gtc_task_inplace_create_and_add_sdc(gtc_t gtc, task_class_t tclass) {
  tc_t   *tc = gtc_lookup(gtc);
  task_t *t;
  TC_START_TSCTIMER(tc,addinplace);

  //assert(gtc_group_steal_ismember(gtc)); // Only masters can do this

  t = (task_t*) sdc_shrb_alloc_head(tc->shared_rb);
  gtc_task_set_class(t, tclass);

  t->created_by = _c->rank;
  t->affinity   = 0;
  t->priority   = 0;

  ++tc->tasks_spawned;

  TC_STOP_TSCTIMER(tc,addinplace);

  return t;
}


/**
 * Complete an in-place task creation.  Note, you should not do *ANY* other
 * queue operations until all outstanding in-place creations have finished.
 *
 * @param gtc    Portable reference to the task collection
 * @param task   The pointer that was returned by inplace_create_and_add()
 */
void gtc_task_inplace_create_and_add_finish_sdc(gtc_t gtc, task_t *t) {
  tc_t *tc = gtc_lookup(gtc);
  // TODO: Maintain a counter of how many are outstanding to avoid corruption at the
  // head of the queue
  TC_START_TSCTIMER(tc,addfinish);

  // Can't release until the inplace op completes
  gtc_progress_sdc(gtc);
  TC_STOP_TSCTIMER(tc,addfinish);
}


/**
 * Print stats for this task collection.
 * @param tc       IN Ptr to task collection
 */
void gtc_print_stats_sdc(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);
  sdc_shrb_t *rb = tc->shared_rb;

  uint64_t perget, peradd, perinplace, perfinish, perprogress, perreclaim, perensure, perrelease, perreacquire, perpoptail;

  if (!getenv("SCIOTO_DISABLE_STATS") && !getenv("SCIOTO_DISABLE_PERNODE_STATS")) {
    // avoid floating point exceptions...
    perget       = tc->getcalls      != 0 ? TC_READ_TSCTIMER(tc,getbuf)    / tc->getcalls      : 0;
    peradd       = tc->tasks_spawned != 0 ? TC_READ_TSCTIMER(tc,add)       / tc->tasks_spawned : 0;
    perinplace   = tc->tasks_spawned != 0 ? TC_READ_TSCTIMER(tc,addinplace)/ tc->tasks_spawned : 0; // borrowed
    perfinish    = rb->nprogress     != 0 ? TC_READ_TSCTIMER(tc,addfinish) / rb->nprogress     : 0; // borrowed, but why?
    perprogress  = rb->nprogress     != 0 ? TC_READ_TSCTIMER(tc,progress)  / rb->nprogress     : 0;
    perreclaim   = rb->nreccalls     != 0 ? TC_READ_TSCTIMER(tc,reclaim)   / rb->nreccalls     : 0;
    perensure    = rb->nensure       != 0 ? TC_READ_TSCTIMER(tc,ensure)    / rb->nensure       : 0;
    perrelease   = rb->nrelease      != 0 ? TC_READ_TSCTIMER(tc,release)   / rb->nrelease      : 0;
    perreacquire = rb->nreacquire    != 0 ? TC_READ_TSCTIMER(tc,reacquire) / rb->nreacquire    : 0;
    perpoptail   = rb->ngets         != 0 ? TC_READ_TSCTIMER(tc,poptail)   / rb->ngets         : 0;

    printf(" %4d - SDC-Q: nrelease %6lu, nreacquire %6lu, nreclaimed %6lu, nwaited %2lu, nprogress %6lu\n"
           " %4d -    failed w/lock: %6lu, failed w/o lock: %6lu, aborted steals: %6lu\n"
           " %4d -    ngets: %6lu  (%5.2f usec/get) nxfer: %6lu\n",
      _c->rank,
        tc->shared_rb->nrelease, tc->shared_rb->nreacquire, tc->shared_rb->nreclaimed, tc->shared_rb->nwaited, tc->shared_rb->nprogress,
      _c->rank,
        tc->failed_steals_locked, tc->failed_steals_unlocked, tc->aborted_steals,
      _c->rank,
        tc->shared_rb->ngets, TC_READ_TIMER_USEC(tc, t[0])/(double)tc->shared_rb->ngets, tc->shared_rb->nxfer);
    printf(" %4d - TSC: get: %"PRIu64"M (%"PRIu64" x %"PRIu64")  add: %"PRIu64"M (%"PRIu64" x %"PRIu64") inplace: %"PRIu64"M (%"PRIu64")\n",
        _c->rank,
        TC_READ_TSCTIMER_M(tc,getbuf), perget, tc->getcalls,
        TC_READ_TSCTIMER_M(tc,add), peradd, tc->tasks_spawned,
        TC_READ_TSCTIMER_M(tc,addinplace), perinplace);
    printf(" %4d - TSC: addfinish: %"PRIu64"M (%"PRIu64") progress: %"PRIu64"M (%"PRIu64" x %"PRIu64") reclaim: %"PRIu64"M (%"PRIu64" x %"PRIu64")\n",
        _c->rank,
        TC_READ_TSCTIMER_M(tc,addfinish), perfinish,
        TC_READ_TSCTIMER_M(tc,progress), perprogress, rb->nprogress,
        TC_READ_TSCTIMER_M(tc,reclaim), perreclaim, rb->nreccalls);
    printf(" %4d - TSC: ensure: %"PRIu64"M (%"PRIu64" x %"PRIu64") release: %"PRIu64"M (%"PRIu64" x %"PRIu64") "
           "reacquire: %"PRIu64"M (%"PRIu64" x %"PRIu64")\n",
        _c->rank,
        TC_READ_TSCTIMER_M(tc,ensure), perensure, rb->nensure,
        TC_READ_TSCTIMER_M(tc,release), perrelease, rb->nrelease,
        TC_READ_TSCTIMER_M(tc,reacquire), perreacquire, rb->nreacquire);
    printf(" %4d - TSC: pushhead: %"PRIu64"M (%"PRIu64") poptail: %"PRIu64"M (%"PRIu64" x %"PRIu64")\n",
        _c->rank,
        TC_READ_TSCTIMER_M(tc,pushhead), (uint64_t)0,
        TC_READ_TSCTIMER_M(tc,poptail), perpoptail, rb->ngets);
  }
}



/**
 * Print global stats for this task collection.
 * @param tc       IN Ptr to task collection
 */
void gtc_print_gstats_sdc(gtc_t gtc) {
  char buf1[200], buf2[200], buf3[200];
  double perprogress, perreclaim, perensure, perrelease, perreacquire, perpoptail, pergetmeta;
  tc_t *tc = gtc_lookup(gtc);
  sdc_shrb_t *rb = (sdc_shrb_t *)tc->shared_rb;

  // avoid floating point exceptions...
  perpoptail   = rb->ngets         != 0 ? TC_READ_TSCTIMER_MSEC(tc,poptail)   / rb->ngets         : 0.0;
  pergetmeta   = rb->nmeta         != 0 ? TC_READ_TSCTIMER_MSEC(tc,getmeta)   / rb->nmeta         : 0.0;
  perprogress  = rb->nprogress     != 0 ? TC_READ_TSCTIMER_USEC(tc,progress)  / rb->nprogress     : 0.0;
  perreclaim   = rb->nreccalls     != 0 ? TC_READ_TSCTIMER_USEC(tc,reclaim)   / rb->nreccalls     : 0.0;
  perensure    = rb->nensure       != 0 ? TC_READ_TSCTIMER_USEC(tc,ensure)    / rb->nensure       : 0.0;
  perreacquire = rb->nreacquire    != 0 ? TC_READ_TSCTIMER_MSEC(tc,reacquire) / rb->nreacquire    : 0.0;
  perrelease   = rb->nrelease      != 0 ? TC_READ_TSCTIMER_USEC(tc,release)   / rb->nrelease      : 0.0;

  eprintf("        : gets         %-32stime %35s per %s\n",
      gtc_print_mmau(buf1, "", rb->ngets, 1),
      gtc_print_mmad(buf2, "ms", TC_READ_TSCTIMER_MSEC(tc,poptail), 0),
      gtc_print_mmad(buf3, "ms", perpoptail, 0));
  eprintf("        :   get_buf    %-32s\n", gtc_print_mmau(buf1, "", tc->getcalls, 1));
  eprintf("        :   get_meta   %-32stime %35s per %s\n", 
      gtc_print_mmau(buf1, "", rb->nmeta, 1),
      gtc_print_mmad(buf2, "ms", TC_READ_TSCTIMER_MSEC(tc,getmeta), 0),
      gtc_print_mmad(buf3, "ms", pergetmeta, 0));
  eprintf("        :   localget   %-32s\n", gtc_print_mmau(buf1, "", tc->getlocal, 1));
  eprintf("        :   steals     %-32s\n", gtc_print_mmau(buf1, "", rb->nsteals, 1));
  eprintf("        :   fails lock %-32s\n", gtc_print_mmau(buf1, "", tc->failed_steals_locked, 1));
  eprintf("        :   fails un   %-32s\n", gtc_print_mmau(buf1, "", tc->failed_steals_unlocked, 1));
  eprintf("        :   fails ab   %-32s\n", gtc_print_mmau(buf1, "", tc->aborted_steals, 1));
  eprintf("        : progress   %32s  time %35s    per %s\n",
      gtc_print_mmau(buf1, "", rb->nprogress, 0),
      gtc_print_mmad(buf2, "us", TC_READ_TSCTIMER_USEC(tc,progress), 0),
      gtc_print_mmad(buf3, "us", perprogress, 0));
  eprintf("        : reclaim    %32s  time %35s    per %s\n",
      gtc_print_mmau(buf1, "", rb->nreccalls, 0),
      gtc_print_mmad(buf2, "us", TC_READ_TSCTIMER_USEC(tc,reclaim), 0),
      gtc_print_mmad(buf3, "us", perreclaim, 0));
  eprintf("        : ensure     %32s  time %35s    per %s\n",
      gtc_print_mmau(buf1, "", rb->nensure, 0),
      gtc_print_mmad(buf2, "us", TC_READ_TSCTIMER_USEC(tc,ensure), 0),
      gtc_print_mmad(buf3, "us", perensure, 0));
  eprintf("        : reacquire  %32s  time %35s    per %s\n",
      gtc_print_mmau(buf1, "", rb->nreacquire, 0),
      gtc_print_mmad(buf2, "ms", TC_READ_TSCTIMER_MSEC(tc,reacquire), 0),
      gtc_print_mmad(buf3, "ms", perreacquire, 0));
  eprintf("        : release    %32s  time %35s    per %s\n",
      gtc_print_mmau(buf1, "", rb->nrelease, 0),
      gtc_print_mmad(buf2, "us", TC_READ_TSCTIMER_USEC(tc,release), 0),
      gtc_print_mmad(buf3, "us", perrelease, 0));


#if 0
  unsigned long tgets, tRelease, tReacquire, tProgress, tContFail, tContTot, contention_fails, contention_total;
  double tgettime, mingettime, maxgettime, gettime, perget;

  gtc_reduce(&rb->nrelease,   &tRelease,    GtcReduceOpSum, LongType, 1);
  gtc_reduce(&rb->nreacquire, &tReacquire,  GtcReduceOpSum, LongType, 1);
  gtc_reduce(&rb->nprogress,  &tProgress,   GtcReduceOpSum, LongType, 1);

  contention_fails = tc->failed_steals_locked + tc->aborted_steals;
  contention_total = tc->num_steals + tc->failed_steals_unlocked + tc->failed_steals_locked + tc->aborted_steals;

  gtc_reduce(&contention_fails, &tContFail, GtcReduceOpSum, LongType, 1);
  gtc_reduce(&contention_total, &tContTot, GtcReduceOpSum, LongType, 1);

  gettime = TC_READ_TIMER_USEC(tc, t[0]);
  perget   = gettime / (double)rb->ngets;
  gtc_reduce(&rb->ngets,      &tgets,       GtcReduceOpSum, LongType, 1);
  gtc_reduce(&gettime,        &tgettime,    GtcReduceOpSum, DoubleType, 1);
  gtc_reduce(&perget,         &mingettime,  GtcReduceOpMin, DoubleType, 1);
  gtc_reduce(&perget,         &maxgettime,  GtcReduceOpMax, DoubleType, 1);

  eprintf("   SDC:  nrelease: %6lu nreacq: %6lu, contention %lu/%lu (%5.2f %%) steal attempts\n",
      tRelease, tReacquire, tContFail, tContTot, tContFail/(double)tContTot*100.0);
  eprintf("   SDC:  avg release: %6lu avg reacq: %6lu avg progress: %6lu\n", tRelease/_c->size, tReacquire/_c->size, tProgress/_c->size);
  eprintf("   SDC:  gettime: avg: %5.2f us max: %5.2f us min: %5.2f us\n", tgettime/(double)tgets, maxgettime, mingettime);

#endif
}



/**
 * Delete all tasks in my patch of the task collection.  Useful when
 * simulating failure.
 */
void gtc_queue_reset_sdc(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  // Clear out the ring buffer
  sdc_shrb_lock(tc->shared_rb, _c->rank);
  sdc_shrb_reset(tc->shared_rb);
  sdc_shrb_unlock(tc->shared_rb, _c->rank);

#if 0 /* no task pushing */
  // Clear out the inbox
  shrb_lock(tc->inbox, _c->rank);
  shrb_reset(tc->inbox);
  shrb_unlock(tc->inbox, _c->rank);
#endif /* no task pushing */
}
