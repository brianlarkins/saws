/***********************************************************/
/*                                                         */
/*  common.c - scioto openshmem shared TC operations       */
/*    (c) 2020 see COPYRIGHT in top-level                  */
/*                                                         */
/***********************************************************/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <alloca.h>
#include <math.h>

#include <tc.h>

void gtc_print_my_stats(gtc_t gtc);
//static int dcomp(const void *a, const void *b);

enum target_types_e { FREE, LOCAL_SEARCH, RETRY };

char *target_methods[2] = { "Random", "Round Robin" };
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

    UNUSED(chunk_size);

    // baseline
    // aborting steals and steal half with split queues

    if (!gtc_is_initialized) {
        gtc_is_initialized = -1; // signify that we are initializing sciotwo implicitly
        gtc_init();
    }

    //_c->total_tcs++; //why was this being called here and in gtc_handle_register

    if (!gtc_is_seeded) {
        struct timespec t = gtc_get_wtime();
        unsigned rseed = (1000000000L * t.tv_sec) + t.tv_nsec;
        srand(rseed + _c->rank); // seed randomness (for stealing)
        gtc_is_seeded = 1;
    }

    // allocate collection
    tc = shmem_calloc(1, sizeof(tc_t));
    assert(tc != NULL);

    // add to global registry
    gtc = gtc_handle_register(tc);


    // allocate timers
    tc->timers = calloc(1, sizeof(tc_timers_t));
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

    TC_INIT_TIMER(tc, getbuf);
    TC_INIT_TIMER(tc, add);
    TC_INIT_TIMER(tc, addinplace);
    TC_INIT_TIMER(tc, addfinish);
    TC_INIT_TIMER(tc, progress);
    TC_INIT_TIMER(tc, reclaim);
    TC_INIT_TIMER(tc, ensure);
    TC_INIT_TIMER(tc, release);
    TC_INIT_TIMER(tc, reacquire);
    TC_INIT_TIMER(tc, pushhead);
    TC_INIT_TIMER(tc, poptail);
    TC_INIT_TIMER(tc, getsteal);
    TC_INIT_TIMER(tc, getfail);
    TC_INIT_TIMER(tc, getmeta);

    if (!ldbal_cfg) {
        ldbal_cfg = alloca(sizeof(gtc_ldbal_cfg_t));
        gtc_ldbal_cfg_init(ldbal_cfg);
    }

    switch (qtype) {
        case GtcQueueSDC:
        case GtcQueueSAWS:
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
        case GtcQueueSAWS:
            gtc_create_saws(gtc, max_body_size, shrb_size, ldbal_cfg);
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

    shmem_free(tc);

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
    tc->ct.tasks_completed = 0;
    tc->ct.tasks_spawned   = 0;
    tc->ct.tasks_stolen    = 0;
    tc->ct.num_steals      = 0;
    tc->ct.passive_count   = 0;
    tc->ct.failed_steals_locked   = 0;
    tc->ct.failed_steals_unlocked = 0;
    tc->ct.aborted_steals         = 0;
    tc->ct.aborted_targets        = 0;
    tc->ct.dispersion_attempts_unlocked = 0;
    tc->ct.dispersion_attempts_locked   = 0;

    // Reset round-robin counter
    tc->last_target = (_c->rank + 1) % _c->size;

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
        idx += snprintf(msg+idx, size-idx, ", Target selection: %s", target_methods[tc->ldbal_cfg.target_selection]);

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
    UNUSED(priority);
    return tc->rcb.pop_head(tc->shared_rb, _c->rank, buf);
}

/**
 * gtc_steal_tail -- Attempt to steal a chunk of tasks from the given target's tail.
 *
 * @param  target        Process ID of target
 * @param  req_stealsize Amount of work to steal.
 * @return number of tasks stolen
 */
int gtc_steal_tail(gtc_t gtc, int target) {
    tc_t *tc = gtc_lookup(gtc);
    int   stealsize;
    int   req_stealsize;
    tc_timer_t temp;

    if (tc->ldbal_cfg.steal_method == STEAL_CHUNK)
        req_stealsize = tc->ldbal_cfg.chunk_size;
    else
        req_stealsize = __GTC_MAX_STEAL_SIZE;

    TC_INIT_ATIMER(temp);
    TC_START_ATIMER(temp);
    stealsize = tc->rcb.pop_n_tail(tc->shared_rb, target, req_stealsize, tc->steal_buf, tc->ldbal_cfg.steal_method);
    TC_STOP_ATIMER(temp);

    // account into success or failed steal timers
    if (stealsize > 0)
        TC_ADD_TIMER(tc, getsteal, temp);
    else
        TC_ADD_TIMER(tc, getfail, temp);

    if (stealsize > 0) {
        gtc_lprintf(DBGGET, "\tthread %d: steal try: %d got: %d tasks from thread %d\n", _c->rank, req_stealsize, stealsize, target);
        tc->rcb.push_n_head(tc->shared_rb, _c->rank, tc->steal_buf, stealsize);

    } else if (stealsize < 0) {
        //gtc_lprintf(DBGGET, "\tthread %d: Aborting steal from %d\n", _c->rank, target);
        // XXX should account for number of aborted steals?
    } else {
        //gtc_lprintf(DBGGET, "\tthread %d: failed steal got no tasks from thread %d\n", _c->rank, target);
    }

    return stealsize;
}


/**
 * gtc_try_steal_tail -- Attempt to steal a chunk of tasks from the given target's tail.
 *
 * @param  target        Process ID of target
 * @param  req_stealsize Amount of work to steal.  If this number is negative, perform work
 *                       splitting and get up to abs(req_stealsize) tasks.
 * @return number of tasks stolen or -1 on failure
 */
int gtc_try_steal_tail(gtc_t gtc, int target) {
    tc_t *tc = gtc_lookup(gtc);
    int   stealsize;
    int   req_stealsize;

    if (tc->ldbal_cfg.steal_method == STEAL_CHUNK)
        req_stealsize = tc->ldbal_cfg.chunk_size;
    else
        req_stealsize = __GTC_MAX_STEAL_SIZE;

    gtc_lprintf(DBGGET, "attempting to steal from %d\n", target);

#ifdef QUEUE_TRY_POP_N_TAIL
    if (tc->qtype == GtcQueueSAWS)
        stealsize = tc->cb.try_pop_n_tail(tc->shrb, target, req_stealsize, tc->steal_buf, tc->ldbal_cfg.      steal_method);
    else
        stealsize = tc->cb.try_pop_n_tail(tc->shared_rb, target, req_stealsize, tc->steal_buf, tc->ldbal_cfg.         steal_method);


#else
    stealsize = tc->rcb.pop_n_tail(tc->shared_rb, target, req_stealsize, tc->steal_buf, tc->ldbal_cfg.         steal_method);
#endif


    if (stealsize > 0) {
        gtc_lprintf(DBGGET, "stole %d tasks from %d\n", stealsize, target);
        tc->rcb.push_n_head(tc->shared_rb, _c->rank, tc->steal_buf, stealsize);
    } else if (stealsize < 0) {
        gtc_lprintf(DBGGET, "aborting steal from %d\n", target);
    }

    //gtc_lprintf(DBGGET, "steal completed\n");

    return stealsize;
}



/** Internal target selector state machine: Select the next target to attempt a steal from.
 *
 * @param[in] gtc   Current task collection
 * @param[in] state State of the target selector.  The state struct should be
 *                  initially set to 0
 * @return          Next target
 */
int gtc_select_target(gtc_t gtc, gtc_vs_state_t *state) {
    int v;
    tc_t *tc;

    tc = gtc_lookup(gtc);
    v  = -1;

    /* SINGLE: Single processor run
    */
    if (_c->size == 1) {
        v = 0;
    }

    /* RETRY: Attempt to steal from the same target again.  This is used
     * with aborting steals which are non-blocking a require retrying.
     */
    if (state->target_retry) {
        // Note: max_steal_retries < 0 means infinite number of retries
        if (state->num_retries >= tc->ldbal_cfg.max_steal_retries && tc->ldbal_cfg.max_steal_retries > 0) {
            state->num_retries = 0;
            tc->ct.aborted_targets++;

        } else {
            state->target_retry = 0;
            state->num_retries++;
            v = state->last_target;
        }
    }

    /* FREE: Free target selection.
    */
    if (v < 0) {
        // Target Random: Randomly select the next target
        if (tc->ldbal_cfg.target_selection == TARGET_RANDOM) {
            do {
                v = rand() % _c->size;
            } while (v == _c->rank);
        }

        // Round Robin: Next target is selected round-robin
        else if (tc->ldbal_cfg.target_selection == TARGET_ROUND_ROBIN) {
            v = (state->last_target + 1) % _c->size;
        }

        else {
            printf("Unknown target selection method\n");
            assert(0);
        }
    }

    state->last_target = v;

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
        //static int getcount = 0;
        gtc_task_execute(gtc, &xtask->task);
        //gtc_dprintf("executed task %d\n", ++getcount);
    }
    free(xtask);
    tc->state = STATE_TERMINATED;
    TC_STOP_TIMER(tc, process);

#ifdef GTC_TRACE
    shmem_barrier_all();
    gtc_trace_stop();
    fclose(trace_file);
#endif

    assert(gtc_tasks_avail(gtc) == 0);
}

typedef enum {
    ProcessTime,
    PassiveTime,
    SearchTime,
    AcquireTime,
    DispersionTime,
    ImbalanceTime
} gtc_gtimestats_e;


typedef enum {
    TasksCompleted,
    TasksStolen,
    NumSteals,
    DispersionAttempts
} gtc_gcountstats_e;



/**
 * Print stats for this task collection.
 * @param tc       IN Ptr to task collection
 */
void gtc_print_gstats(gtc_t gtc) {
    char *stats_disabled         = getenv("SCIOTO_DISABLE_STATS");
    char *pernode_stats_disabled = getenv("SCIOTO_DISABLE_PERNODE_STATS");
    char *ext_stats_enabled      = getenv("SCIOTO_EXTENDED_STATS");
    char *unordered_stats         = getenv("SCIOTO_UNORDERED_STATS");
    tc_t *tc = gtc_lookup(gtc);
    double   *times, *mintimes, *maxtimes, *sumtimes;
    uint64_t *counts, *mincounts, *maxcounts, *sumcounts;


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

    int ntimes = 7;
    times     = shmem_calloc(ntimes, sizeof(double));
    mintimes  = shmem_calloc(ntimes, sizeof(double));
    maxtimes  = shmem_calloc(ntimes, sizeof(double));
    sumtimes  = shmem_calloc(ntimes, sizeof(double));

    int ncounts = 4;
    counts     = shmem_calloc(ncounts, sizeof(uint64_t));
    mincounts  = shmem_calloc(ncounts, sizeof(uint64_t));
    maxcounts  = shmem_calloc(ncounts, sizeof(uint64_t));
    sumcounts  = shmem_calloc(ncounts, sizeof(uint64_t));

    assert(times && mintimes && maxtimes && sumtimes);
    assert(counts && mincounts && maxcounts && sumcounts);
    memset(mintimes, 0, ntimes*sizeof(double));
    memset(maxtimes, 0, ntimes*sizeof(double));
    memset(sumtimes, 0, ntimes*sizeof(double));


    times[ProcessTime]    = TC_READ_TIMER_SEC(tc,process);
    times[PassiveTime]    = TC_READ_TIMER_SEC(tc,passive);
    times[SearchTime]     = TC_READ_TIMER_SEC(tc,search);
    times[AcquireTime]    = tc->ct.num_steals ? (TC_READ_TIMER_SEC(tc,passive) - TC_READ_TIMER_SEC(tc, imbalance))/tc->ct.num_steals : 0.0;
    times[DispersionTime] = TC_READ_TIMER_SEC(tc,dispersion);
    times[ImbalanceTime]  = TC_READ_TIMER_SEC(tc,imbalance);

    counts[TasksCompleted]     = tc->ct.tasks_completed;
    counts[TasksStolen]        = tc->ct.tasks_stolen;
    counts[NumSteals]          = tc->ct.num_steals;

    shmem_min_reduce(SHMEM_TEAM_WORLD, mintimes, times, ntimes);
    shmem_max_reduce(SHMEM_TEAM_WORLD, maxtimes, times, ntimes);
    shmem_sum_reduce(SHMEM_TEAM_WORLD, sumtimes, times, ntimes);

    shmem_min_reduce(SHMEM_TEAM_WORLD, mincounts, counts, ncounts);
    shmem_max_reduce(SHMEM_TEAM_WORLD, maxcounts, counts, ncounts);
    shmem_sum_reduce(SHMEM_TEAM_WORLD, sumcounts, counts, ncounts);
    shmem_barrier_all();

    if (ext_stats_enabled == NULL) {
        eprintf("Total  : stolen %3lu, steals %3lu Average: stolen %3lu, steals %3lu\n",
                sumcounts[TasksStolen], sumcounts[NumSteals],
                sumcounts[TasksStolen]/_c->size, sumcounts[NumSteals]/_c->size);
        eprintf("Time   : worst dispersion %8.5fms, worst imbalance %8.5fms, best imbalance %8.5fms,"
                " avg acquire %8.5fms, avg search %8.5fs (%5.2f %%)\n",
                maxtimes[DispersionTime]*1000.0,
                maxtimes[ImbalanceTime]*1000.0,
                mintimes[ImbalanceTime]*1000.0,
                sumtimes[AcquireTime]/(_c->size*1000.0),
                sumtimes[SearchTime]/(_c->size),
                sumtimes[SearchTime]/sumtimes[PassiveTime]);
        tc->cb.print_gstats(gtc);
    }

    fflush(NULL);
    shmem_barrier_all();

    struct timespec sleep = { 0, 25 * 1000000L }; // 25ms
    nanosleep(&sleep, NULL);       // lag a little to make sure that final results appear after all other stats i/o is done.

    eprintf("SCIOTO : Process time %.5f s, passive time %.5f s (%.2f%%), %lu tasks completed, %.2f tasks/sec (%.2f tasks/sec/PE)\n",
            sumtimes[ProcessTime]/_c->size, sumtimes[PassiveTime]/_c->size,
            (sumtimes[PassiveTime]/(sumtimes[ProcessTime])*100.0),
            sumcounts[TasksCompleted]/(sumtimes[ProcessTime]/_c->size),
            sumcounts[TasksCompleted]/sumtimes[ProcessTime]);

    fflush(NULL);

    shmem_free(times);
    shmem_free(mintimes);
    shmem_free(maxtimes);
    shmem_free(sumtimes);

    shmem_free(counts);
    shmem_free(mincounts);
    shmem_free(maxcounts);
    shmem_free(sumcounts);

    shmem_barrier_all();
}



void gtc_print_stats(gtc_t gtc) {
    char *stats_disabled         = getenv("SCIOTO_DISABLE_STATS");
    char *pernode_stats_disabled = getenv("SCIOTO_DISABLE_PERNODE_STATS");
    char *unordered_stats        = getenv("SCIOTO_UNORDERED_STATS");
    tc_t *tc = gtc_lookup(gtc);
    double   *times, *mintimes, *maxtimes, *sumtimes;
    uint64_t *counts, *mincounts, *maxcounts, *sumcounts;

    if (stats_disabled) return;

    shmem_barrier_all();
    if (!pernode_stats_disabled) {
        if (unordered_stats) {
            gtc_print_my_stats(gtc);
        } else {
            for (int i=0; i<= _c->size; i++) {
                if (i == _c->rank)
                    gtc_print_my_stats(gtc);
                shmem_barrier_all();
            }
        }
    }
    fflush(NULL);
    shmem_barrier_all();

    int ntimes = 7;
    times     = shmem_calloc(ntimes, sizeof(double));
    mintimes  = shmem_calloc(ntimes, sizeof(double));
    maxtimes  = shmem_calloc(ntimes, sizeof(double));
    sumtimes  = shmem_calloc(ntimes, sizeof(double));

    int ncounts = 4;
    counts     = shmem_calloc(ncounts, sizeof(uint64_t));
    mincounts  = shmem_calloc(ncounts, sizeof(uint64_t));
    maxcounts  = shmem_calloc(ncounts, sizeof(uint64_t));
    sumcounts  = shmem_calloc(ncounts, sizeof(uint64_t));

    assert(times && mintimes && maxtimes && sumtimes);
    assert(counts && mincounts && maxcounts && sumcounts);
    memset(mintimes, 0, ntimes*sizeof(double));
    memset(maxtimes, 0, ntimes*sizeof(double));
    memset(sumtimes, 0, ntimes*sizeof(double));


    times[ProcessTime]    = TC_READ_TIMER_SEC(tc,process);
    times[PassiveTime]    = TC_READ_TIMER_SEC(tc,passive);
    times[SearchTime]     = TC_READ_TIMER_MSEC(tc,search);
    times[AcquireTime]    = tc->ct.num_steals ? (TC_READ_TIMER_MSEC(tc,passive) - TC_READ_TIMER_MSEC(tc, imbalance))/tc->ct.num_steals : 0.0;
    times[DispersionTime] = TC_READ_TIMER_MSEC(tc,dispersion);
    times[ImbalanceTime]  = TC_READ_TIMER_MSEC(tc,imbalance);

    counts[TasksCompleted]     = tc->ct.tasks_completed;
    counts[TasksStolen]        = tc->ct.tasks_stolen;
    counts[NumSteals]          = tc->ct.num_steals;
    counts[DispersionAttempts] = tc->ct.dispersion_attempts_locked + tc->ct.dispersion_attempts_unlocked;

    shmem_min_reduce(SHMEM_TEAM_WORLD, mintimes, times, ntimes);
    shmem_max_reduce(SHMEM_TEAM_WORLD, maxtimes, times, ntimes);
    shmem_sum_reduce(SHMEM_TEAM_WORLD, sumtimes, times, ntimes);

    shmem_min_reduce(SHMEM_TEAM_WORLD, mincounts, counts, ncounts);
    shmem_max_reduce(SHMEM_TEAM_WORLD, maxcounts, counts, ncounts);
    shmem_sum_reduce(SHMEM_TEAM_WORLD, sumcounts, counts, ncounts);
    shmem_barrier_all();


    // 9 char to colon
    eprintf("process: %.5f : %.5f size: %d\n", sumtimes[ProcessTime], times[ProcessTime], _c->size);
    eprintf("SCIOTWO : queue: %s \n", gtc_queue_name(gtc));
    eprintf("        : process time %.5f s, passive time %.5f s (%.2f%%), search time %.5f ms\n",
            sumtimes[ProcessTime]/_c->size, sumtimes[PassiveTime]/_c->size, (sumtimes[PassiveTime]/sumtimes[ProcessTime])*100.0,
            sumtimes[SearchTime]/_c->size);
    eprintf("        : tasks completed %lu, %.2f tasks/sec (%.2f tasks/sec/PE)\n",
            sumcounts[TasksCompleted],
            sumtimes[TasksCompleted]/(sumtimes[ProcessTime]/_c->size),
            sumcounts[TasksCompleted]/sumtimes[ProcessTime]);

    eprintf("        : dispersion %6.2fms/%6.2fms/%6.2fms attempts %6.2f (%6.2f/%6.2f/%6.2f)\n",
            sumtimes[DispersionTime]/_c->size, mintimes[DispersionTime], maxtimes[DispersionTime],
            sumcounts[DispersionAttempts], sumcounts[DispersionAttempts]/_c->size,
            mincounts[DispersionAttempts], maxcounts[DispersionAttempts]);

    eprintf("        : imbalance  %6.2fms/%6.2fms/%6.2fms\n",
            sumtimes[ImbalanceTime]/_c->size, mintimes[ImbalanceTime], maxtimes[ImbalanceTime]);

    tc->cb.print_gstats(gtc);

    //this is the graph data.
    // num_processes, average_time, passive_time, search_time, average_dispersion_time, dispersion_attempts, tasks_completed, idk
    eprintf("&&&&  %lu      %.5f %lu %.5f %.5f %6.2f %6.2f %.2f %.2f\n", _c->size, sumtimes[ProcessTime]/_c->size,      
        sumtimes[PassiveTime]/_c->size, sumtimes[SearchTime]/_c->size, sumtimes[DispersionTime]/_c->size,
        sumcounts[DispersionAttempts]/_c->size,
        sumcounts[TasksCompleted], sumtimes[TasksCompleted]/(sumtimes[ProcessTime]/_c->size));

    shmem_free(times);
    shmem_free(mintimes);
    shmem_free(maxtimes);
    shmem_free(sumtimes);

    shmem_free(counts);
    shmem_free(mincounts);
    shmem_free(maxcounts);
    shmem_free(sumcounts);

    fflush(NULL);
    shmem_barrier_all();
}




void gtc_print_my_stats(gtc_t gtc) {
    double        avg_acquire_time_ms;
    tc_t         *tc = gtc_lookup(gtc);

    char *stats_disabled         = getenv("SCIOTO_DISABLE_STATS");
    char *pernode_stats_disabled = getenv("SCIOTO_DISABLE_PERNODE_STATS");

    if (stats_disabled) return;

    avg_acquire_time_ms = (TC_READ_TIMER(tc, passive) - TC_READ_TIMER(tc, imbalance))/(tc->ct.num_steals * (double)10e6);

    if (!pernode_stats_disabled) {
        printf(" %4d - Tasks: completed %3lu, spawned %3lu, stolen %3lu\n"
                " %4d -      : nsteals %3lu, steal fails -- %3lu, aborted %3lu\n"
                " %4d -  Time: process %8.5fs = active %8.5fs + passive %8.5fs, passive count=%lu (%8.5fms avg), searching %8.5fs (%5.2f %%)\n"
                " %4d -      : dispersion %8.5fms, disp. steal fails -- unlocked %3lu, locked %3lu\n"
                " %4d -      : avg acquire %8.5fms, imbalance %8.5fms\n"
                " %4d -      : timers: %8.5fms %8.5fms %8.5fms %8.5f %8.5fms\n",
                _c->rank,
                tc->ct.tasks_completed, tc->ct.tasks_spawned, tc->ct.tasks_stolen,
                _c->rank,
                tc->ct.num_steals, tc->ct.failed_steals_unlocked + tc->ct.failed_steals_locked, tc->ct.aborted_steals,
                _c->rank,
                TC_READ_TIMER_SEC(tc, process),
                TC_READ_TIMER_SEC(tc, process) - TC_READ_TIMER_SEC(tc, passive),
                TC_READ_TIMER_SEC(tc, passive),
                tc->ct.passive_count,
                TC_READ_TIMER_MSEC(tc, passive) / tc->ct.passive_count,
                TC_READ_TIMER_SEC(tc, search),
                (TC_READ_TIMER(tc, search)/(double)TC_READ_TIMER(tc, process))*100.0,
                _c->rank,
                TC_READ_TIMER_MSEC(tc, dispersion),
                tc->ct.dispersion_attempts_unlocked,
                tc->ct.dispersion_attempts_locked,
                _c->rank,
                (tc->ct.num_steals > 0) ?  avg_acquire_time_ms : 0.6, TC_READ_TIMER_MSEC(tc, imbalance),
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
    return tc->ct.tasks_completed;
}


/** Query the number of tasks that the local process has spawned.
*/
unsigned long gtc_stats_tasks_spawned(gtc_t gtc) {
    tc_t *tc = gtc_lookup(gtc);
    return tc->ct.tasks_spawned;
}

