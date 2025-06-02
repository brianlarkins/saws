/***********************************************************/
/*                                                         */
/*  collection-laws.c - scioto openshmem lock-based TC impl */
/*    (c) 2021 see COPYRIGHT in top-level                  */
/*                                                         */
/***********************************************************/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tc.h"

#include "laws_shrb.h"

#define PROCID 128
#define ATTEMPTS 10
// #define STEAL_CNT 32
// #define LAWS_ENABLE
// #undef LAWS_ENABLE
//       #include "shr_ring.h"

/**
 * Create a new task collection.  Collective call.
 *
 * @param[in] max_body_size Max size of a task descriptor's body in bytes for
 * this tc. Any task that is added must be smaller or equal to this size.
 * @param[in] shrb_size    Size of the local task queue (in tasks).
 * @param[in] cfg          Load balancer configuation.  NULL for default
 * configuration.
 *
 * @return                 Portable task collection handle.
 */
gtc_t gtc_create_laws(gtc_t gtc, int max_body_size, int shrb_size,
                      gtc_ldbal_cfg_t *cfg) {
  GTC_ENTRY();
  tc_t *tc;

  UNUSED(max_body_size);
  UNUSED(cfg);

  tc = gtc_lookup(gtc);

  // Allocate the shared ring buffer.  Total task size is the size
  // of the header + max_body size.
  tc->shared_rb =
      laws_create(tc->max_body_size + sizeof(task_t), shrb_size, tc);
  // tc->inbox = shrb_create(tc->max_body_size + sizeof(task_t), shrb_size);
  tc->inbox = NULL;

  tc->cb.destroy = gtc_destroy_laws;
  tc->cb.reset = gtc_reset_laws;
  tc->cb.get_buf = gtc_get_buf_laws;
  tc->cb.add = gtc_add_laws;
  tc->cb.inplace_create_and_add = gtc_task_inplace_create_and_add_laws;
  tc->cb.inplace_ca_finish = gtc_task_inplace_create_and_add_finish_laws;
  tc->cb.progress = gtc_progress_laws;
  tc->cb.tasks_avail = gtc_tasks_avail_laws;
  tc->cb.queue_name = gtc_queue_name_laws;
  tc->cb.print_stats = gtc_print_stats_laws;
  tc->cb.print_gstats = gtc_print_gstats_laws;

  tc->rcb.pop_head = laws_pop_head;
  tc->rcb.pop_n_tail = laws_pop_n_tail;
  tc->rcb.try_pop_n_tail = laws_try_pop_n_tail;
  tc->rcb.push_n_head = laws_push_n_head;
  tc->rcb.work_avail = laws_size;

  tc->qsize = sizeof(laws_t);

  shmem_barrier_all();

  GTC_EXIT(gtc);
}

/**
 * Destroy task collection.  Collective call.
 */
void gtc_destroy_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);

  laws_destroy(tc->shared_rb);
  GTC_EXIT();
}

/**
 * Reset a task collection so it can be reused and removes any remaining tasks.
 * Collective call.
 */
void gtc_reset_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  laws_reset(tc->shared_rb);
  GTC_EXIT();
}

/**
 * String that gives the name of this queue
 */
char *gtc_queue_name_laws() {
  GTC_ENTRY();
#ifdef laws_NODC
  GTC_EXIT("Split (NODC)");
#else
  GTC_EXIT("Split Deferred-Copy");
#endif
}

/** Invoke the progress engine.  Update work queues, balance the schedule,
 *  make progress on communication.
 */
void gtc_progress_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  TC_START_TIMER(tc, progress);
  laws_t *local_md = (laws_t *)tc->shared_rb;

  // printf("entering gtc progress\n");
#if 0  /* no task pushing */
  // Check the inbox for new work
  if (shrb_size(tc->inbox) > 0) {
    int   ntasks, npopped;
    void *work;

    ntasks = 100;
    work   = gtc_malloc((tc->max_body_size+sizeof(task_t))*ntasks);
    npopped= shrb_pop_n_tail(tc->inbox, _c->rank, ntasks, work, STEAL_CHUNK);

    laws_shrb_push_n_head(tc->shared_rb, _c->rank, work, npopped);
    shrb_free(work);

    gtc_lprintf(DBGINBOX, "gtc_progress: Moved %d tasks from inbox to my queue\n", npopped);
  }
#endif /* no task pushing */

  // Update the split
  laws_release(tc->shared_rb);

  // Attempt to reclaim space
  laws_reclaim_space(tc->shared_rb);

  // check for work from bitfield; if work available, set flag
  // if (local_md->procid == local_md->root)
  // printf("%lu\n", *local_md->global_bits);
  // int num_tasks = laws_size(local_md);
  /*if (num_tasks >= 32) {*/
  /*  shmem_atomic_or(local_md->global_bits, local_md->our_bits, 0);*/
  /*} else {*/
  /*  shmem_atomic_and(local_md->global_bits, local_md->our_invert, 0);*/
  /*}*/
  // int total_tasks = 0;
  int cores_with_tasks = 0;
  // int node_num;
  /*printf("local_md->procid: %d\n", local_md->procid);*/
  /*printf("local_md->root: %d\n", local_md->root);*/
  /*printf("local_md->ncores: %d\n", local_md->ncores);*/
  /*printf("local_md->nnodes: %d\n", local_md->nnodes);*/
  if (local_md->procid == local_md->root) {
    *local_md->gb_copy &= *local_md->global_bits;
    *local_md->gb_copy |= *local_md->global_bits;
    /*local_md->num_tasks_per_core[0] = num_tasks;*/
    /*for (int i = 0; i < local_md->ncores; i++) {*/
    /*  total_tasks += local_md->num_tasks_per_core[i];*/
    /*}*/
    uint64_t new_bits = *local_md->global_bits;
    while (new_bits) {
      if (new_bits & 1)
        // total_tasks += STEAL_CNT;
        cores_with_tasks++;
      new_bits >>= 1;
    }
    /*if (local_md->procid != 0) {*/
    /*  shmem_atomic_set(&local_md->num_tasks_per_node[local_md->node_num],*/
    /*                   total_tasks, 0);*/
    /*} else {*/
    /*  local_md->num_tasks_per_node[0] = total_tasks;*/
    /*}*/
    /*for (int i = 0; i < local_md->nnodes; i++) {*/
    /*  // printf("1\n");*/
    /*  printf("%d |", local_md->num_tasks_per_node[i]);*/
    /*}*/
    // if (total_tasks >= 256)
    if (cores_with_tasks >= (local_md->ncores / 2))
      *(local_md->has_work_avail) = 1;
    else
      *(local_md->has_work_avail) = 0;
  } else {
    /*shmem_atomic_set(&local_md->num_tasks_per_core[local_md->rank],
     * num_tasks,*/
    /*                 local_md->root);*/
  }

#if 0
  shmem_quiet();
  if (local_md->procid == 0) {
    printf("\n");
    for (int i = 0; i < local_md->nnodes; i++) {
      printf("%d\t", local_md->num_tasks_per_node[i]);
    }
    printf("\n");
    /*for (int i = 0; i < local_md->ncores; i++) {*/
    /*  printf("%d\t", local_md->num_tasks_per_core[i]);*/
    /*}*/
    /*printf("\n");*/
  }
#endif
  ((laws_t *)tc->shared_rb)->nprogress++;
  TC_STOP_TIMER(tc, progress);
  GTC_EXIT();
}

/**
 * Number of tasks available in local task collection.  Note, this is an
 * approximate number since we're not locking the data structures.
 */
int gtc_tasks_avail_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);

  // return laws_shrb_size(tc->shared_rb) + shrb_size(tc->inbox);
  GTC_EXIT(laws_size(tc->shared_rb));
}

static inline int is_local(int v, laws_t *rb) {
  if (v >= rb->root && v < rb->root + rb->ncores)
    return 1;
  return 0;
}

static inline char *print_bits(uint64_t num) {
  static char bitstring[65];
  char cur_str[2];
  bitstring[64] = '\0';
  for (int i = 0; i < 64; i++) {
    // num >>= 1;
    sprintf(cur_str, "%lu", (num & 1));
    bitstring[64 - (i + 1)] = cur_str[0];
    num >>= 1;
  }
  return bitstring;
}

static inline void increment_success(tc_t *tc) {
  double curr_time = TC_READ_TIMER_MSEC(tc, passive);
  laws_t *local_md = (laws_t *)tc->shared_rb;

  int new_time = (int)curr_time;
  local_md->successes[new_time]++;
  local_md->curr_success++;
  local_md->curr_ratio =
      (double)local_md->curr_success / (double)local_md->curr_fails;
  local_md->ratio[new_time] = local_md->curr_ratio;
  local_md->local_successes[new_time] = local_md->local_success;
}

static inline void increment_fail(tc_t *tc) {
  double curr_time = TC_READ_TIMER_MSEC(tc, passive);
  laws_t *local_md = (laws_t *)tc->shared_rb;

  int new_time = (int)curr_time;
  local_md->fails[new_time]++;
  local_md->curr_fails++;
  local_md->curr_ratio =
      (double)local_md->curr_success / (double)local_md->curr_fails;
  local_md->ratio[new_time] = local_md->curr_ratio;
  local_md->local_successes[new_time] = local_md->local_success;
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
double gtc_get_dummy_work_laws = 0.0;

/** Internal target selector state machine: Select the next target to attempt a
 * steal from.
 *
 * @param[in] gtc   Current task collection
 * @param[in] state State of the target selector.  The state struct should be
 *                  initially set to 0
 * @return          Next target
 */
int gtc_select_target_laws(gtc_t gtc, gtc_vs_state_t *state) {
  GTC_ENTRY();
  int v = -1;
  tc_t *tc = gtc_lookup(gtc);
  laws_t *local_md = (laws_t *)tc->shared_rb;

  // TODO: check our node, then check other nodes to determine whether they have
  // work available
  // uint64_t chosen_bits = 0;
  int rand_node;
  int root = local_md->root;
  int attempts = 0;
  // if (local_md->procid == 2) {
  uint64_t gb_copy = 0;
  shmem_getmem(&gb_copy, local_md->gb_copy, 1, local_md->root);
  // printf("approx: %s\n", print_bits(gb_copy));
  //}
  // TC_START_TIMER(tc, atomic_get);
  shmem_atomic_fetch(local_md->global_bits, local_md->root);
  // printf("%d: %s\n", local_md->procid, print_bits(*local_md->global_bits));
  if (local_md->procid == 2) {
    // printf("actual: %s\n", print_bits(*local_md->global_bits));
  }
  // TC_STOP_TIMER(tc, atomic_get);
  // tc->ct.atomic_gets++;
  //  printf("%lu\n", *local_md->global_bits);
  while (!*local_md->global_bits && attempts < ATTEMPTS) {
    rand_node = rand() % local_md->nnodes;
    root = rand_node * local_md->ncores;
    if (root != local_md->root) {
      TC_START_TIMER(tc, atomic_get);
      shmem_atomic_fetch(local_md->global_bits, root);
      TC_STOP_TIMER(tc, atomic_get);
      tc->ct.atomic_gets++;
    }
    // printf("%d : %s\n", local_md->procid,
    // print_bits(*local_md->global_bits));
    attempts++;
  }
  if (attempts == ATTEMPTS) {
    v = gtc_select_target(gtc, state);
    GTC_EXIT(v);
  }
  // printf("Done!\n");
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
    if (state->num_retries >= tc->ldbal_cfg.max_steal_retries &&
        tc->ldbal_cfg.max_steal_retries > 0) {
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
      // printf("local_md->root: %d\n", local_md->root);
      // uint64_t num_two = *local_md->global_bits;
      // for (int i = 0; i < 64; i++) {
      //   num_two = *local_md->global_bits << i;
      //   printf("%lu", (num_two & 0x1));
      // }
      // printf("\n");
      do {
        v = rand() % local_md->ncores;
        uint64_t new_num = *local_md->global_bits >> v;
        int i;
        for (i = v; (new_num & 1) == 0; i = (i + 1) % local_md->ncores) {
          new_num = *local_md->global_bits >> i;
          // printf("%d\n", i);
        }
        v = i;
      } while (v == local_md->rank);
    }

    // Round Robin: Next target is selected round-robin
    else if (tc->ldbal_cfg.target_selection == TARGET_ROUND_ROBIN) {
      v = (state->last_target + 1) % local_md->ncores;
    }

    else {
      printf("Unknown target selection method\n");
      assert(0);
    }
  }

  state->last_target = v;

  GTC_EXIT(v + root);
}

int gtc_get_buf_laws_2(gtc_t gtc, int priority, task_t *buf) {
  // 1). check to see whether we have work locally
  // 2). if not, attempt to steal from another process
  //      - start by checking for work available locally
  //          (have an array of metadata)
  //          (reduces memory which is grabbed)
  //
  // possible rewrite incoming! (depends on how I feel about the progress for
  // this thing...)

  // tc_t *tc = gtc_lookup(gtc);
  return 0;
}

int gtc_get_buf_laws(gtc_t gtc, int priority, task_t *buf) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  int got_task = 0;
  int v, steal_size;
  int passive = 0;
  int searching = 0;
  int increment_avg = 0;
  // int perc_local = 2;
  gtc_vs_state_t vs_state = {0, 0, 0};
  laws_t rb_buf;
  laws_t *local_md = (laws_t *)tc->shared_rb;
  // uint64_t node_has_work;
  // uint8_t node_cpy;

  tc->ct.getcalls++;
  TC_START_TIMER(tc, getbuf);
  tc->ct.steals_at_run = 0;
  tc->ct.runs++;
  tc->ct.this_run = 0;

  // Invoke the progress engine
  gtc_progress(gtc);

  // Try to take my own work first.  We take from the head of our own queue.
  // When we steal, we take work off of the tail of the target's queue.
  got_task = gtc_get_local_buf(gtc, priority, buf);

  // Time dispersion.  If I had work to start this should be ~0.
  if (!tc->dispersed)
    TC_START_TIMER(tc, dispersion);

  // No more local work, try to steal some
  if (!got_task && tc->ldbal_cfg.stealing_enabled) {
    increment_avg = 1;
    gtc_lprintf(DBGGET, " Thread %d: gtc_get() searching for work\n", _c->rank);

#ifndef NO_SEATBELTS
    TC_START_TIMER(tc, passive);
    TC_INIT_TIMER(tc, imbalance);
    TC_START_TIMER(tc, imbalance);
    passive = 1;
    tc->ct.passive_count++;
#endif

    vs_state.last_target = tc->last_target;

    // Keep searching until we find work or detect termination
    while (!got_task && !tc->terminated) {
      int max_steal_attempts, steal_attempts, steal_done;
      void *target_rb = &rb_buf;

      tc->state = STATE_SEARCHING;

      if (!searching) {
        TC_START_TIMER(tc, search);
        searching = 1;
      }

#ifndef LAWS_ENABLE
      // v = (rand() % 100);
      v = gtc_select_target(gtc, &vs_state);

      // only try getting locally if we've already successfully stolen work
      // prior
      // if ((v <= perc_local || local_md->local_success) && tc->dispersed) {
      //
      // shmem_atomic_fetch(local_md->global_bits, local_md->root);

      // printf("%lu\n", node_has_work);
#else
      // uint8_t node_cpy;
      // shmem_getmem(&node_cpy, local_md->has_work_avail, 1, local_md->root);
      // shmem_getmem(&local_md->gb_copy, local_md->
      // shmem_atomic_fetch(local_md->global_bits, local_md->root);
      // uint64_t num_again = *local_md->global_bits;
      // if (*local_md->global_bits) {
      //   printf("%lu\n", *local_md->global_bits);
      //   for (int i = 0; i < 64; i++) {
      //     num_again = *local_md->global_bits >> i;
      //     printf("%lu", (num_again & 1));
      //   }
      //   printf("\n");
      // }
      // if (tc->dispersed && local_md->local_success) {
      // if (local_md->procid % 2 == 0 && tc->dispersed) {
      // if (*local_md->global_bits && tc->dispersed) {
      //   v = gtc_select_target_laws(gtc, &vs_state);
      //   v += local_md->root;
      // } else {
      //   v = gtc_select_target(gtc, &vs_state);
      // }
      if (tc->dispersed)
        v = gtc_select_target_laws(gtc, &vs_state);
      else
        v = gtc_select_target(gtc, &vs_state);
#endif
      /*if (local_md->procid % 3 == 0) {*/
      /*  v = gtc_select_target(gtc, &vs_state);*/
      /*} else {*/
      /*  v = gtc_select_target_laws(gtc, &vs_state);*/
      /*  v += local_md->root;*/
      /*}*/

      /*if (local_md->procid == local_md->root)*/
      /*  printf("%lu\n", *local_md->global_bits);*/
      /*else*/
      /*  printf("%u\n", node_cpy);*/
      max_steal_attempts = tc->ldbal_cfg.max_steal_attempts_remote;

      TC_START_TIMER(tc, poptail); // this counts as attempting to steal
      shmem_getmem(target_rb, tc->shared_rb, sizeof(laws_t), v);
      TC_STOP_TIMER(tc, poptail);

      // Poll the target for work.  In between polls, maintain progress on
      // termination detection.
      for (steal_attempts = 0, steal_done = 0;
           !steal_done && !tc->terminated &&
           steal_attempts < max_steal_attempts;
           steal_attempts++) {

        // Apply linear backoff to avoid flooding remote nodes
        // TODO: do we need this if we are stealing on-node?
        if (steal_attempts > 0) {
          int j;
          for (j = 0; j < steal_attempts * 1000; j++)
            gtc_get_dummy_work_laws += 1.0;
        }

        if (tc->rcb.work_avail(target_rb) > 0) {
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
          // printf("%d : %d\n", steal_size, v);

          local_md->curr_task_avg =
              local_md->curr_task_avg +
              ((double)(steal_size - local_md->curr_task_avg) /
               (tc->ct.total_steals + 1));

          if (local_md->procid == 35)
            // printf("%g\n", local_md->curr_task_avg);
            // printf("%d\n", steal_size);

            if (!local_md->sdc_back) {
              if (local_md->curr_task_avg > local_md->sdc_avg &&
                  !local_md->local_success) {
                local_md->sdc_avg = local_md->curr_task_avg;
              } else if (!local_md->local_success) {
                local_md->local_success = 1;
                local_md->sdc_back = 1;
              }
            }
          // printf("%g\n", local_md->curr_task_avg);
          tc->ct.this_run++;
          tc->ct.total_steals++;
          if (tc->ct.this_run >= tc->ct.last_run + 25 &&
              local_md->local_success) {
            //
            // Idea: check that this_run is not substantially greater than
            // last_run
            // - if it is, we're unlikely to get any work anytime soon
            // if (tc->ct.this_run >= tc->ct.last_run * 5 &&
            //    local_md->local_success) {
            local_md->local_success = 0;
          }
          if (local_md->procid == PROCID) {
            /*printf("%ld     %ld     %d      %d      %d\n", tc->ct.this_run,*/
            /*       tc->ct.last_run, tc->dispersed, steal_size,*/
            /*       local_md->local_success);*/
          }
          if (local_md->local_success) {
            tc->ct.steals_at_run++;
            tc->ct.avg_steals_per_run =
                (double)(tc->ct.avg_steals_per_run * (tc->ct.runs - 1) +
                         tc->ct.steals_at_run) /
                (tc->ct.runs);
          }

          // Steal succeeded: Got some work from remote node
          if (steal_size > 0) {
            // printf("%d\n", steal_size);
            // increment number of tasks stolen per node
            if (!is_local(v, local_md))
              shmem_atomic_add(local_md->num_tasks_stolen, steal_size,
                               local_md->root);
            tc->ct.tasks_stolen += steal_size;
            tc->ct.num_steals++;
            increment_success(tc);
            // if (!local_md->has_work)
            local_md->has_work = 1;
            // increment this if the steal was local
            if (is_local(v, local_md)) {
              // local_md->local_successes[local_md->local_idx]++;
              tc->ct.num_local_steals++; // numbr of successful local steals
              // if (local_md->procid == 100 && !local_md->local_success)
              //   printf("local_success: 1\n");
              // if (local_md->procid == 100)
              //   printf("curr_ratio: %g\n", local_md->curr_ratio);
              // local_md->local_success = 1;
            }
            steal_done = 1;
            tc->last_target = v;

            // Steal failed: Got the lock, no longer any work on remote node
          } else if (steal_size == 0) {
            /*if (local_md->procid == 100 && tc->ct.this_run < 50) {*/
            /*  printf("%ld       %ld       %d\n", tc->ct.this_run,*/
            /*         tc->ct.last_run, tc->dispersed);*/
            /*}*/
            tc->ct.failed_steals_locked++;
            // printf("%u\n", node_cpy);
            increment_fail(tc);
            // if the steal failed and we attempted locally, set flag off
            if (is_local(v, local_md)) {
              // if (local_md->procid == 100 && local_md->local_success)
              //   printf("local_success: 0\n");
              // printf("curr_ratio: %g\n", local_md->curr_ratio);
              // local_md->local_success = 0;
              // local_md->local_idx++;
            }
            steal_done = 1;

            // Steal aborted: Didn't get the lock, refresh target metadata and
            // try again
          } else {
            if (steal_attempts + 1 == max_steal_attempts) {
              tc->ct.aborted_steals++;
            }
            vs_state.target_retry = 1;
          }

        } else /* ! (QUEUE_WORK_AVAIL(target_rb) > 0) */ {
          tc->ct.failed_steals_unlocked++;
          /*if (local_md->procid == local_md->root)*/
          /*  printf("%lu\n", *local_md->global_bits);*/
          /*else*/
          /*  printf("%u\n", node_cpy);*/
          steal_done = 1;
          tc->ct.this_run++;
          tc->ct.total_steals++;
          if (tc->ct.this_run >= tc->ct.last_run + 25 &&
              local_md->local_success) {
            local_md->local_success = 0;
          }
          if (local_md->procid == PROCID) {
            /*printf("%ld       %ld       %d      %d\n", tc->ct.this_run,*/
            /*       tc->ct.last_run, tc->dispersed, local_md->local_success);*/
          }
          if (is_local(v, local_md)) {
            // if (local_md->procid == 100 && local_md->local_success)
            //   printf("local_success: 0\n");
            // local_md->local_success = 0;
            //   printf("curr_ratio: %g\n", local_md->curr_ratio);
          }
          increment_fail(tc);
        }

        // Invoke the progress engine
        gtc_progress(gtc);

        // Still no work? Lock to be sure and check for termination.
        // Locking is only needed here if we allow pushing.
        // TODO: New TD should not require locking.  Remove locks and test.
        if (gtc_tasks_avail(gtc) == 0 && !tc->external_work_avail) {
          // QUEUE_LOCK(tc->shared_rb, _c->rank);
          // shrb_lock(tc->inbox, _c->rank); /* no task pushing */
          if (gtc_tasks_avail(gtc) == 0 && !tc->external_work_avail) {
            td_set_counters(tc->td, tc->ct.tasks_spawned,
                            tc->ct.tasks_completed);
            tc->terminated = td_attempt_vote(tc->td);
          }

          // shrb_unlock(tc->inbox, _c->rank); /* no task pushing */
          // QUEUE_UNLOCK(tc->shared_rb, _c->rank);

          // We have work, done stealing
        } else {
          steal_done = 1;
        }
      }

      if (gtc_tasks_avail(gtc))
        got_task = gtc_get_local_buf(gtc, priority, buf);
    }

  } else {
    tc->ct.getlocal++;
  }

#ifndef NO_SEATBELTS
  if (passive)
    TC_STOP_TIMER(tc, passive);
  if (passive)
    TC_STOP_TIMER(tc, imbalance);
  if (searching)
    TC_STOP_TIMER(tc, search);
#endif

  // Record how many attempts it took for our first get, this is the number of
  // attempts during the work dispersion phase.
  if (!tc->dispersed) {
    if (passive)
      TC_STOP_TIMER(tc, dispersion);
    tc->dispersed = 1;
    double curr_time = TC_READ_TIMER_MSEC(tc, passive);
    int new_time = (int)curr_time;
    local_md->dispersion_mark[new_time] = 1;

    tc->ct.dispersion_attempts_unlocked = tc->ct.failed_steals_unlocked;
    tc->ct.dispersion_attempts_locked = tc->ct.failed_steals_locked;

    // local_md->sdc_avg = local_md->curr_task_avg;
  }
  if (increment_avg)
    tc->ct.last_run = tc->ct.this_run;

  /*if (local_md->local_success && increment_avg) {*/
  /*  tc->ct.runs++;*/
  /*  tc->ct.avg_steals_per_run =*/
  /*      (double)((tc->ct.avg_steals_per_run * (tc->ct.runs - 1)) +*/
  /*               (tc->ct.steals_at_run)) /*/
  /*      (tc->ct.runs);*/
  /*  if (local_md->procid == 32) {*/
  /*    printf("%g\n", tc->ct.avg_steals_per_run);*/
  /*  }*/
  /*}*/

  gtc_lprintf(DBGGET, " Thread %d: gtc_get() %s\n", _c->rank,
              got_task ? "got work" : "no work");

  if (got_task)
    tc->state = STATE_WORKING;
  TC_STOP_TIMER(tc, getbuf);
  GTC_EXIT(got_task);
}

/**
 * Add task to the task collection.  Task is copied in and task buffer is
 * available to the user when call returns.  Non-collective call.
 *
 * @param tc       IN Ptr to task collection
 * @param proc     IN Process # whose task collection this task is to be added
 *                    to. Common case is tc->procid
 * @param task  INOUT Task to be added. user manages buffer when call
 *                    returns. Preferably allocated in ARMCI local allocated
 * memory when proc != tc->procid for improved RDMA performance.  This call
 * fills in task field and the contents of task will match what is in the queue
 *                    when the call returns.
 *
 * @return 0 on success.
 */
int gtc_add_laws(gtc_t gtc, task_t *task, int proc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);

  assert(gtc_task_body_size(task) <= tc->max_body_size);
  assert(tc->state != STATE_TERMINATED);
  TC_START_TIMER(tc, add);

  task->created_by = _c->rank;

  if (proc == _c->rank) {
    // Local add: put it straight onto the local work list
    laws_push_head(tc->shared_rb, _c->rank, task,
                   sizeof(task_t) + gtc_task_body_size(task));
  }
#if 0  /* no task pushing */
  else {
    // Remote adds: put this in the remote node's inbox
    if (task->affinity == 0)
      shrb_push_head(tc->inbox, proc, task, sizeof(task_t) + gtc_task_body_size(task));
    else
      shrb_push_tail(tc->inbox, proc, task, sizeof(task_t) + gtc_task_body_size(task));
  }
#endif /* no task pushing */

  ++tc->ct.tasks_spawned;
  TC_STOP_TIMER(tc, add);

  GTC_EXIT(0);
}

/**
 * Create-and-add a task in-place on the head of the queue.  Note, you should
 * not do *ANY* other queue operations until all outstanding in-place creations
 * have finished.  The pointer returned points directly to an element in the
 * queue.  Do not add it, do not free it, discard the pointer when you are
 * finished assigning the task body.
 *
 * @param gtc    Portable reference to the task collection
 * @param tclass Desired task class
 */
task_t *gtc_task_inplace_create_and_add_laws(gtc_t gtc, task_class_t tclass) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  task_t *t;
  TC_START_TIMER(tc, addinplace);

  // assert(gtc_group_steal_ismember(gtc)); // Only masters can do this

  t = (task_t *)laws_alloc_head(tc->shared_rb);
  gtc_task_set_class(t, tclass);

  t->created_by = _c->rank;
  // t->affinity   = 0;
  t->priority = 0;

  ++tc->ct.tasks_spawned;

  TC_STOP_TIMER(tc, addinplace);

  GTC_EXIT(t);
}

/**
 * Complete an in-place task creation.  Note, you should not do *ANY* other
 * queue operations until all outstanding in-place creations have finished.
 *
 * @param gtc    Portable reference to the task collection
 * @param task   The pointer that was returned by inplace_create_and_add()
 */
void gtc_task_inplace_create_and_add_finish_laws(gtc_t gtc, task_t *t) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  UNUSED(t);
  // TODO: Maintain a counter of how many are outstanding to avoid corruption at
  // the head of the queue
  TC_START_TIMER(tc, addfinish);

  // Can't release until the inplace op completes
  gtc_progress_laws(gtc);
  TC_STOP_TIMER(tc, addfinish);
  GTC_EXIT();
}

/**
 * Print stats for this task collection.
 * @param tc       IN Ptr to task collection
 */
void gtc_print_stats_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  laws_t *rb = (laws_t *)tc->shared_rb;

  uint64_t perget, peradd, perinplace, perfinish, perprogress, perreclaim,
      perensure, perrelease, perreacquire, perpoptail;

  // if (rb->procid == 0) {
  //   printf("hi there!\n");
  // }
  if (!getenv("SCIOTO_DISABLE_STATS") &&
      !getenv("SCIOTO_DISABLE_PERNODE_STATS")) {
    // avoid floating point exceptions...
    perget =
        tc->ct.getcalls != 0 ? TC_READ_TIMER(tc, getbuf) / tc->ct.getcalls : 0;
    peradd = tc->ct.tasks_spawned != 0
                 ? TC_READ_TIMER(tc, add) / tc->ct.tasks_spawned
                 : 0;
    perinplace = tc->ct.tasks_spawned != 0
                     ? TC_READ_TIMER(tc, addinplace) / tc->ct.tasks_spawned
                     : 0; // borrowed
    perfinish = rb->nprogress != 0
                    ? TC_READ_TIMER(tc, addfinish) / rb->nprogress
                    : 0; // borrowed, but why?
    perprogress =
        rb->nprogress != 0 ? TC_READ_TIMER(tc, progress) / rb->nprogress : 0;
    perreclaim =
        rb->nreccalls != 0 ? TC_READ_TIMER(tc, reclaim) / rb->nreccalls : 0;
    perensure = rb->nensure != 0 ? TC_READ_TIMER(tc, ensure) / rb->nensure : 0;
    perrelease =
        rb->nrelease != 0 ? TC_READ_TIMER(tc, release) / rb->nrelease : 0;
    perreacquire =
        rb->nreacquire != 0 ? TC_READ_TIMER(tc, reacquire) / rb->nreacquire : 0;
    perpoptail = rb->ngets != 0 ? TC_READ_TIMER(tc, poptail) / rb->ngets : 0;

    printf(" %4d - laws-Q: nrelease %6lu, nreacquire %6lu, nreclaimed %6lu, "
           "nwaited %2lu, nprogress %6lu\n"
           " %4d -    failed w/lock: %6lu, failed w/o lock: %6lu, aborted "
           "steals: %6lu\n"
           " %4d -    ngets: %6lu  (%5.2f usec/get) nxfer: %6lu\n"
           " %4d -    nglobalrets: %6lu (%5.2f usec/get)\n"
           " %4d -    num local steals: %6lu, perc. of local steals: %6g\n",
           _c->rank, rb->nrelease, rb->nreacquire, rb->nreclaimed, rb->nwaited,
           rb->nprogress, _c->rank, tc->ct.failed_steals_locked,
           tc->ct.failed_steals_unlocked, tc->ct.aborted_steals, _c->rank,
           rb->ngets, TC_READ_TIMER_USEC(tc, t[0]) / (double)rb->ngets,
           rb->nxfer, _c->rank, tc->ct.global_ret_count,
           TC_READ_TIMER_USEC(tc, global_ret) / (double)tc->ct.global_ret_count,
           _c->rank, tc->ct.num_local_steals,
           ((double)tc->ct.num_local_steals / (double)tc->ct.num_steals) * 100);
    printf(" %4d - TSC: get: %" PRIu64 "M (%" PRIu64 " x %" PRIu64
           ")  add: %" PRIu64 "M (%" PRIu64 " x %" PRIu64 ") inplace: %" PRIu64
           "M (%" PRIu64 ")\n",
           _c->rank, TC_READ_TIMER_M(tc, getbuf), perget, tc->ct.getcalls,
           TC_READ_TIMER_M(tc, add), peradd, tc->ct.tasks_spawned,
           TC_READ_TIMER_M(tc, addinplace), perinplace);
    printf(" %4d - TSC: addfinish: %" PRIu64 "M (%" PRIu64
           ") progress: %" PRIu64 "M (%" PRIu64 " x %" PRIu64
           ") reclaim: %" PRIu64 "M (%" PRIu64 " x %" PRIu64 ")\n",
           _c->rank, TC_READ_TIMER_M(tc, addfinish), perfinish,
           TC_READ_TIMER_M(tc, progress), perprogress, rb->nprogress,
           TC_READ_TIMER_M(tc, reclaim), perreclaim, rb->nreccalls);
    printf(" %4d - TSC: ensure: %" PRIu64 "M (%" PRIu64 " x %" PRIu64
           ") release: %" PRIu64 "M (%" PRIu64 " x %" PRIu64 ") "
           "reacquire: %" PRIu64 "M (%" PRIu64 " x %" PRIu64 ")\n",
           _c->rank, TC_READ_TIMER_M(tc, ensure), perensure, rb->nensure,
           TC_READ_TIMER_M(tc, release), perrelease, rb->nrelease,
           TC_READ_TIMER_M(tc, reacquire), perreacquire, rb->nreacquire);
    printf(" %4d - TSC: pushhead: %" PRIu64 "M (%" PRIu64 ") poptail: %" PRIu64
           "M (%" PRIu64 " x %" PRIu64 ")\n",
           _c->rank, TC_READ_TIMER_M(tc, pushhead), (uint64_t)0,
           TC_READ_TIMER_M(tc, poptail), perpoptail, rb->ngets);
  }
  GTC_EXIT();
}

/**
 * Print global stats for this task collection.
 * @param tc       IN Ptr to task collection
 */
void gtc_print_gstats_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  laws_t *rb = (laws_t *)tc->shared_rb;
  double *times, *mintimes, *maxtimes, *sumtimes, *avgratio;
  uint64_t *counts, *mincounts, *maxcounts, *sumcounts;

  int ntimes = 20;
  times = gtc_shmem_calloc(ntimes, sizeof(double));
  mintimes = gtc_shmem_calloc(ntimes, sizeof(double));
  maxtimes = gtc_shmem_calloc(ntimes, sizeof(double));
  sumtimes = gtc_shmem_calloc(ntimes, sizeof(double));

  int ncounts = 17;
  counts = gtc_shmem_calloc(ncounts, sizeof(uint64_t));
  mincounts = gtc_shmem_calloc(ncounts, sizeof(uint64_t));
  maxcounts = gtc_shmem_calloc(ncounts, sizeof(uint64_t));
  sumcounts = gtc_shmem_calloc(ncounts, sizeof(uint64_t));

  avgratio = gtc_shmem_calloc(100000, sizeof(double));
  times[LAWSPopTailTime] = TC_READ_TIMER_MSEC(tc, poptail);
  times[LAWSStealTime] = TC_READ_TIMER_MSEC(tc, steal);
  times[LAWSGetMetaTime] = TC_READ_TIMER_MSEC(tc, getmeta);
  times[LAWSProgressTime] = TC_READ_TIMER_USEC(tc, progress);
  times[LAWSReclaimTime] = TC_READ_TIMER_USEC(tc, reclaim);
  times[LAWSEnsureTime] = TC_READ_TIMER_USEC(tc, ensure);
  times[LAWSReacquireTime] = TC_READ_TIMER_MSEC(tc, reacquire);
  times[LAWSReleaseTime] = TC_READ_TIMER_USEC(tc, release);
  times[LAWSGlobalRetTime] = TC_READ_TIMER_MSEC(tc, global_ret);
  times[LAWSPerPopTailTime] =
      rb->ngets != 0 ? TC_READ_TIMER_MSEC(tc, poptail) / rb->ngets : 0.0;
  times[LAWSPerStealTime] =
      tc->ct.num_steals != 0 ? TC_READ_TIMER_USEC(tc, steal) / tc->ct.num_steals
                             : 0.0;
  times[LAWSPerGetMetaTime] =
      rb->nmeta != 0 ? TC_READ_TIMER_MSEC(tc, getmeta) / rb->nmeta : 0.0;
  times[LAWSPerProgressTime] =
      rb->nprogress != 0 ? TC_READ_TIMER_USEC(tc, progress) / rb->nprogress
                         : 0.0;
  times[LAWSPerReclaimTime] =
      rb->nreccalls != 0 ? TC_READ_TIMER_USEC(tc, reclaim) / rb->nreccalls
                         : 0.0;
  times[LAWSPerEnsureTime] =
      rb->nensure != 0 ? TC_READ_TIMER_USEC(tc, ensure) / rb->nensure : 0.0;
  times[LAWSPerReacquireTime] =
      rb->nreacquire != 0 ? TC_READ_TIMER_USEC(tc, reacquire) / rb->nreacquire
                          : 0.0;
  times[LAWSPerReleaseTime] =
      rb->nrelease != 0 ? TC_READ_TIMER_USEC(tc, release) / rb->nrelease : 0.0;
  times[LAWSPerGlobalRetTime] =
      tc->ct.global_ret_count != 0
          ? TC_READ_TIMER_USEC(tc, global_ret) / tc->ct.global_ret_count
          : 0.0;
  times[LAWSAtomicGetTime] = TC_READ_TIMER_MSEC(tc, atomic_get);
  times[LAWSPerAtomicGetTime] =
      tc->ct.atomic_gets != 0
          ? TC_READ_TIMER_USEC(tc, atomic_get) / tc->ct.atomic_gets
          : 0.0;

  counts[LAWSNumGets] = rb->ngets;
  counts[LAWSGetCalls] = tc->ct.getcalls;
  counts[LAWSNumMeta] = rb->nmeta;
  counts[LAWSGetLocalCalls] = tc->ct.getlocal;
  counts[LAWSNumSteals] = tc->ct.num_steals;
  counts[LAWSNumLocalSteals] = tc->ct.num_local_steals;
  counts[LAWSStealFailsLocked] = tc->ct.failed_steals_locked;
  counts[LAWSStealFailsUnlocked] = tc->ct.failed_steals_unlocked;
  counts[LAWSAbortedSteals] = tc->ct.aborted_steals;
  counts[LAWSProgressCalls] = rb->nprogress;
  counts[LAWSReclaimCalls] = rb->nreccalls;
  counts[LAWSEnsureCalls] = rb->nensure;
  counts[LAWSReacquireCalls] = rb->nreacquire;
  counts[LAWSReleaseCalls] = rb->nrelease;
  counts[LAWSGlobalRetCalls] = tc->ct.global_ret_count;
  counts[LAWSNumTasksStolen] = tc->ct.tasks_stolen;
  counts[LAWSNumAtomicGets] = tc->ct.atomic_gets;

  shmem_min_reduce(SHMEM_TEAM_WORLD, mintimes, times, ntimes);
  shmem_max_reduce(SHMEM_TEAM_WORLD, maxtimes, times, ntimes);
  shmem_sum_reduce(SHMEM_TEAM_WORLD, sumtimes, times, ntimes);

  shmem_min_reduce(SHMEM_TEAM_WORLD, mincounts, counts, ncounts);
  shmem_max_reduce(SHMEM_TEAM_WORLD, maxcounts, counts, ncounts);
  shmem_sum_reduce(SHMEM_TEAM_WORLD, sumcounts, counts, ncounts);

  /*
  double percsteals[3];
  percsteals[0] = (((double)sumcounts[LAWSNumLocalSteals]/_c->size) /
  ((double)sumcounts[LAWSNumSteals]/_c->size)) * 100; percsteals[1] =
  (((double)sumcounts[LAWSNumLocalSteals]/_c->size) /
  ((double)sumcounts[LAWSNumSteals]/_c->size)) * 100;
  */

  // calculate the ratio of steal successes to steal failures
  // for (int i = 0; i < 100000; i++) {
  //   if (rb->fails[i] != 0)
  //     rb->ratio[i] = (double)rb->successes[i] / (double)rb->fails[i];
  //   else
  //     rb->ratio[i] = (double)rb->successes[i];
  // }

  shmem_sum_reduce(SHMEM_TEAM_WORLD, avgratio, rb->ratio, 100000);

  for (int i = 0; i < 100000; i++) {
    avgratio[i] /= rb->nproc;
  }

  double percsteals;
  double avg_tasks_per_steal;
  percsteals = ((double)sumcounts[LAWSNumLocalSteals] /
                (double)sumcounts[LAWSNumSteals]) *
               100;

  // How many tasks are stolen on average per steal?
  avg_tasks_per_steal =
      (double)sumcounts[LAWSNumTasksStolen] / (double)sumcounts[LAWSNumSteals];

  shmem_barrier_all();

  eprintf("        : uts elem size : %d\n", rb->elem_size);
  eprintf("        : shared heap memory allocated: %d    local heap memory "
          "allocated: %d\n",
          _c->shmallocsize, _c->allocsize);

  eprintf("        : gets         %6lu (%6.2f/%3lu/%3lu) time "
          "%6.2fms/%6.2fms/%6.2fms per %6.2fms/%6.2fms/%6.2fms\n",
          sumcounts[LAWSNumGets], sumcounts[LAWSNumGets] / (double)_c->size,
          mincounts[LAWSNumGets], maxcounts[LAWSNumGets],
          sumtimes[LAWSPopTailTime] / _c->size, mintimes[LAWSPopTailTime],
          maxtimes[LAWSPopTailTime], sumtimes[LAWSPerPopTailTime] / _c->size,
          mintimes[LAWSPerPopTailTime], maxtimes[LAWSPerPopTailTime]);

  eprintf("        :   get_buf    %6lu (%6.2f/%3lu/%3lu\n",
          sumcounts[LAWSGetCalls], sumcounts[LAWSGetCalls] / (double)_c->size,
          mincounts[LAWSGetCalls], maxcounts[LAWSGetCalls]);

  eprintf("        :   get_meta   %6lu (%6.2f/%3lu/%3lu) time "
          "%6.2fms/%6.2fms/%6.2fms per %6.2fms/%6.2fms/%6.2fms\n",
          sumcounts[LAWSNumMeta], sumcounts[LAWSNumMeta] / (double)_c->size,
          mincounts[LAWSNumMeta], maxcounts[LAWSNumMeta],
          sumtimes[LAWSGetMetaTime] / _c->size, mintimes[LAWSGetMetaTime],
          maxtimes[LAWSGetMetaTime], sumtimes[LAWSPerGetMetaTime] / _c->size,
          mintimes[LAWSPerGetMetaTime], maxtimes[LAWSPerGetMetaTime]);

  eprintf("        :   atomic_gets   %6lu (%6.2f/%3lu/%3lu) time "
          "%6.2fms/%6.2fms/%6.2fms per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSNumAtomicGets],
          sumcounts[LAWSNumAtomicGets] / (double)_c->size,
          mincounts[LAWSNumAtomicGets], maxcounts[LAWSNumAtomicGets],
          sumtimes[LAWSAtomicGetTime] / _c->size, mintimes[LAWSAtomicGetTime],
          maxtimes[LAWSAtomicGetTime],
          sumtimes[LAWSPerAtomicGetTime] / _c->size,
          mintimes[LAWSPerAtomicGetTime], maxtimes[LAWSPerAtomicGetTime]);
  eprintf("        :   get_global   %6lu (%6.2f/%3lu/%3lu) time "
          "%6.2fms/%6.2fms/%6.2fms per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSGlobalRetCalls],
          sumcounts[LAWSGlobalRetCalls] / (double)_c->size,
          mincounts[LAWSGlobalRetCalls], maxcounts[LAWSGlobalRetCalls],
          sumtimes[LAWSGlobalRetTime] / _c->size, mintimes[LAWSGlobalRetTime],
          maxtimes[LAWSGlobalRetTime],
          sumtimes[LAWSPerGlobalRetTime] / _c->size,
          mintimes[LAWSPerGlobalRetTime], maxtimes[LAWSPerGlobalRetTime]);
  eprintf("        :   localget   %6lu (%6.2f/%3lu/%3lu)\n",
          sumcounts[LAWSGetLocalCalls],
          sumcounts[LAWSGetLocalCalls] / (double)_c->size,
          mincounts[LAWSGetLocalCalls], maxcounts[LAWSGetLocalCalls]);
  eprintf("        :   steals     %6lu (%6.2f/%3lu/%3lu)\n",
          sumcounts[LAWSNumSteals], sumcounts[LAWSNumSteals] / (double)_c->size,
          mincounts[LAWSNumSteals], maxcounts[LAWSNumSteals]);
  eprintf("        :   local steals     %6lu (%6.2f/%3lu/%3lu) perc. of steals "
          "overall %6g\n",
          sumcounts[LAWSNumLocalSteals],
          sumcounts[LAWSNumLocalSteals] / (double)_c->size,
          mincounts[LAWSNumLocalSteals], maxcounts[LAWSNumLocalSteals],
          percsteals);
  eprintf("        :   get_tasks   time %6.2fms/%6.2fms/%6.2fms per "
          "%6.2fus/%6.2fus/%6.2fus; avg. tasks per steal: %6.2f\n",

          sumtimes[LAWSStealTime] / _c->size, mintimes[LAWSStealTime],
          maxtimes[LAWSStealTime], sumtimes[LAWSPerStealTime] / _c->size,
          mintimes[LAWSPerStealTime], maxtimes[LAWSPerStealTime],
          avg_tasks_per_steal);
  eprintf("        :   fails lock %6lu (%6.2f/%3lu/%3lu)\n",
          sumcounts[LAWSStealFailsLocked],
          sumcounts[LAWSStealFailsLocked] / (double)_c->size,
          mincounts[LAWSStealFailsLocked], maxcounts[LAWSStealFailsLocked]);
  eprintf("        :   fails un   %6lu (%6.2f/%3lu/%3lu)\n",
          sumcounts[LAWSStealFailsUnlocked],
          sumcounts[LAWSStealFailsUnlocked] / (double)_c->size,
          mincounts[LAWSStealFailsUnlocked], maxcounts[LAWSStealFailsUnlocked]);
  eprintf("        :   fails ab   %6lu (%6.2f/%3lu/%3lu)\n",
          sumcounts[LAWSAbortedSteals],
          sumcounts[LAWSAbortedSteals] / (double)_c->size,
          mincounts[LAWSAbortedSteals], maxcounts[LAWSAbortedSteals]);

  eprintf("        : progress   %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus "
          "per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSProgressCalls] / (double)_c->size,
          mincounts[LAWSProgressCalls], maxcounts[LAWSProgressCalls],
          sumtimes[LAWSProgressTime] / _c->size, mintimes[LAWSProgressTime],
          maxtimes[LAWSProgressTime], sumtimes[LAWSPerProgressTime] / _c->size,
          mintimes[LAWSPerProgressTime], maxtimes[LAWSPerProgressTime]);
  eprintf("        : reclaim    %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus "
          "per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSReclaimCalls] / (double)_c->size,
          mincounts[LAWSReclaimCalls], maxcounts[LAWSReclaimCalls],
          sumtimes[LAWSReclaimTime] / _c->size, mintimes[LAWSReclaimTime],
          maxtimes[LAWSReclaimTime], sumtimes[LAWSPerReclaimTime] / _c->size,
          mintimes[LAWSPerReclaimTime], maxtimes[LAWSPerReclaimTime]);
  eprintf("        : ensure     %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus "
          "per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSEnsureCalls] / (double)_c->size,
          mincounts[LAWSEnsureCalls], maxcounts[LAWSEnsureCalls],
          sumtimes[LAWSEnsureTime] / _c->size, mintimes[LAWSEnsureTime],
          maxtimes[LAWSEnsureTime], sumtimes[LAWSPerEnsureTime] / _c->size,
          mintimes[LAWSPerEnsureTime], maxtimes[LAWSPerEnsureTime]);
  eprintf("        : reacquire  %6.2f/%3lu/%3lu time %6.2fms/%6.2fms/%6.2fms "
          "per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSReacquireCalls] / (double)_c->size,
          mincounts[LAWSReacquireCalls], maxcounts[LAWSReacquireCalls],
          sumtimes[LAWSReacquireTime] / _c->size, mintimes[LAWSReacquireTime],
          maxtimes[LAWSReacquireTime],
          sumtimes[LAWSPerReacquireTime] / _c->size,
          mintimes[LAWSPerReacquireTime], maxtimes[LAWSPerReacquireTime]);
  eprintf("        : release    %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus "
          "per %6.2fus/%6.2fus/%6.2fus\n",
          sumcounts[LAWSReleaseCalls] / (double)_c->size,
          mincounts[LAWSReleaseCalls], maxcounts[LAWSReleaseCalls],
          sumtimes[LAWSReleaseTime] / _c->size, mintimes[LAWSReleaseTime],
          maxtimes[LAWSReleaseTime], sumtimes[LAWSPerReleaseTime] / _c->size,
          mintimes[LAWSPerReleaseTime], maxtimes[LAWSPerReleaseTime]);

  eprintf("&&&  %6.2f %6.2f ", sumtimes[LAWSPopTailTime] / _c->size,
          sumtimes[LAWSReacquireTime] / _c->size);

  // shmem_barrier_all();
  // if (rb->procid == rb->root) {
  //   printf("%d : %d\n", rb->procid, *rb->num_tasks_stolen);
  // }

  /*for (int i = 0; i < 1000; i++) {*/
  /*  if (rb->procid == 100) {*/
  /*    // printf("%d    %d    %g\n", rb->successes[i], rb->fails[i],*/
  /*    // rb->ratio[i]);*/
  /*    printf("%g (local success: %d)", rb->ratio[i],
   * rb->local_successes[i]);*/
  /*    if (rb->dispersion_mark[i])*/
  /*      printf("(dispersed)");*/
  /*    printf("\n");*/
  /*  }*/
  /*for (int i = 0; i < rb->local_idx; i++) {*/
  /*  if (rb->procid == 100) {*/
  /*    printf("%d\n", rb->local_successes[i]);*/
  /*  }*/
  /*}*/
  /*}*/
  // for (int i = 0; i < 1000; i++) {
  //     eprintf
  // }
  // for (int i = 0; i < 1000; i++) {
  //   eprintf("%g\n", avgratio[i]);
  // }

  shmem_free(times);
  shmem_free(mintimes);
  shmem_free(maxtimes);
  shmem_free(sumtimes);

  shmem_free(counts);
  shmem_free(mincounts);
  shmem_free(maxcounts);
  shmem_free(sumcounts);

  shmem_free(avgratio);
  GTC_EXIT();
}

/**
 * Delete all tasks in my patch of the task collection.  Useful when
 * simulating failure.
 */
void gtc_queue_reset_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);

  // Clear out the ring buffer
  laws_lock(tc->shared_rb, _c->rank);
  laws_reset(tc->shared_rb);
  laws_unlock(tc->shared_rb, _c->rank);

#if 0  /* no task pushing */
  // Clear out the inbox
  shrb_lock(tc->inbox, _c->rank);
  shrb_reset(tc->inbox);
  shrb_unlock(tc->inbox, _c->rank);
#endif /* no task pushing */
  GTC_EXIT();
}
