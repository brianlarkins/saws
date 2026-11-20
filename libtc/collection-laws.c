/*********************************************************/
/*                                                       */
/*  collection-laws.c - task collection on the two-door  */
/*                      SAWS queue (LAWS M2)             */
/*                                                       */
/*********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "tc.h"
#include "laws_shrb.h"

/*
 * Same structure as collection-saws.c (which stays untouched, so the SAWS baselines do not
 * move).  Differences: the queue is laws_shrb_t; progress also runs the maintenance check;
 * a search records failed node-mates per door; stats add a two-door line.
 */

gtc_t gtc_create_laws(gtc_t gtc, int max_body_size, int shrb_size, gtc_ldbal_cfg_t *cfg) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);

  UNUSED(max_body_size);
  UNUSED(cfg);

  tc->shared_rb = laws_shrb_create(tc->max_body_size + sizeof(task_t), shrb_size, tc);
  tc->inbox = NULL;
  tc->laws_door = LAWS_DOOR_BOTH;

  tc->cb.destroy                = gtc_destroy_laws;
  tc->cb.reset                  = gtc_reset_laws;
  tc->cb.get_buf                = gtc_get_buf_laws;
  tc->cb.add                    = gtc_add_laws;
  tc->cb.inplace_create_and_add = gtc_task_inplace_create_and_add_laws;
  tc->cb.inplace_ca_finish      = gtc_task_inplace_create_and_add_finish_laws;
  tc->cb.progress               = gtc_progress_laws;
  tc->cb.tasks_avail            = gtc_tasks_avail_laws;
  tc->cb.queue_name             = gtc_queue_name_laws;
  tc->cb.print_stats            = gtc_print_stats_laws;
  tc->cb.print_gstats           = gtc_print_gstats_laws;

  tc->rcb.pop_head              = laws_shrb_pop_head;
  tc->rcb.pop_n_tail            = laws_shrb_pop_n_tail;
  tc->rcb.try_pop_n_tail        = laws_shrb_try_pop_n_tail;
  tc->rcb.push_n_head           = laws_shrb_push_n_head;
  tc->rcb.work_avail            = laws_shrb_size;

  tc->qsize = sizeof(laws_shrb_t);

  shmem_barrier_all();
  GTC_EXIT(gtc);
}


void gtc_destroy_laws(gtc_t gtc) {
  GTC_ENTRY();
  laws_shrb_destroy(gtc_lookup(gtc)->shared_rb);
  GTC_EXIT();
}


void gtc_reset_laws(gtc_t gtc) {
  GTC_ENTRY();
  laws_shrb_reset(gtc_lookup(gtc)->shared_rb);
  GTC_EXIT();
}


char *gtc_queue_name_laws() {
  GTC_ENTRY();
  GTC_EXIT("LAWS two-door");
}


/** progress: release (an event), damped reclaim, and every laws_poll calls while a door may hold
 *  public work, the maintenance check (bits, refill, re-split).  The per-task cost beyond SAWS
 *  is a flag test and an increment. */
void gtc_progress_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  laws_shrb_t *rb = (laws_shrb_t *)tc->shared_rb;
  TC_START_HTIMER(tc, progress);

  if (laws_shrb_size(rb) > 1)
    laws_shrb_release(rb);

  if ((rb->progresscc++ % rb->reclaimfreq) == 0)
    laws_shrb_reclaim_space(rb);

  if (rb->maint && (rb->far_live | rb->near_live | tc->bit_set | tc->bit_set_near)
      && ((++rb->lawscc & tc->laws_poll_mask) == 0))
    laws_shrb_maintain(rb);

  rb->nprogress++;
  TC_STOP_HTIMER(tc, progress);
  GTC_EXIT();
}


int gtc_tasks_avail_laws(gtc_t gtc) {
  GTC_ENTRY();
  GTC_EXIT(laws_shrb_size(gtc_lookup(gtc)->shared_rb));
}


/** Find work: local first, then steal (see gtc_get_buf_saws).  Returns 0 only at termination. */
int gtc_get_buf_laws(gtc_t gtc, int priority, task_t *buf) {
  GTC_ENTRY();
  tc_t   *tc = gtc_lookup(gtc);
  laws_shrb_t *rb = (laws_shrb_t *)tc->shared_rb;
  int     got_task = 0;
  int     v, steal_size;
  int     passive = 0;
  int     searching = 0;
  gtc_vs_state_t vs_state = {0};

  tc->ct.getcalls++;
  TC_START_HTIMER(tc, getbuf);

  gtc_progress(gtc);
  got_task = gtc_get_local_buf(gtc, priority, buf);

  if (!tc->dispersed) TC_START_TIMER(tc, dispersion);

  if (!got_task && tc->ldbal_cfg.stealing_enabled) {
#ifndef NO_SEATBELTS
    TC_START_TIMER(tc, passive);
    TC_INIT_TIMER(tc, imbalance);
    TC_START_TIMER(tc, imbalance);
    passive = 1;
    tc->ct.passive_count++;
#endif

    laws_shrb_dry(rb);   // start of a search: an event for the door bits

    while (!got_task && !tc->terminated) {
      tc->state = STATE_SEARCHING;
      if (!searching) {
        TC_START_TIMER(tc, search);
        searching = 1;
      }

      tc->laws_door = LAWS_DOOR_BOTH;   // policies without door bits: near door, then far door
      v = gtc_select_target(gtc, &vs_state);
      tc->state = STATE_STEALING;

      steal_size = gtc_steal_tail(gtc, v);
      if (steal_size > 0) {
        tc->ct.tasks_stolen += steal_size;
        tc->ct.num_steals += 1;
        tc->last_target = v;
        searching = 1;
      } else {
        tc->ct.failed_steals_unlocked++;
        if (gtc_is_near(v)) {
          uint64_t bit = 1ULL << _c->loc.node_rank_of[v];
          int doors = rb->last_doors ? rb->last_doors : LAWS_DOOR_BOTH;
          tc->ct.failed_steals_unlocked_near++;
          if (doors & LAWS_DOOR_NEAR) vs_state.near_miss     |= bit;  // skip that door for the rest of this search
          if (doors & LAWS_DOOR_FAR)  vs_state.near_miss_far |= bit;
        }
      }

      gtc_progress(gtc);

      if (gtc_tasks_avail(gtc) == 0 && !tc->external_work_avail) {
        td_set_counters(tc->td, tc->ct.tasks_spawned, tc->ct.tasks_completed);
        tc->terminated = td_attempt_vote(tc->td);
      } else if (gtc_tasks_avail(gtc)) {
        got_task = gtc_get_local_buf(gtc, priority, buf);
      }
    }
  } else {
    tc->ct.getlocal++;
  }

#ifndef NO_SEATBELTS
  if (passive) TC_STOP_TIMER(tc, passive);
  if (passive) TC_STOP_TIMER(tc, imbalance);
#endif

  if (!tc->dispersed) {
    tc->dispersed = 1;
    tc->ct.dispersion_attempts_unlocked = tc->ct.failed_steals_unlocked;
    tc->ct.dispersion_attempts_locked   = tc->ct.failed_steals_locked;
  }

  if (got_task) tc->state = STATE_WORKING;
  TC_STOP_HTIMER(tc, getbuf);
  GTC_EXIT(got_task);
}


int gtc_add_laws(gtc_t gtc, task_t *task, int proc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);

  assert(gtc_task_body_size(task) <= tc->max_body_size);
  assert(tc->state != STATE_TERMINATED);
  assert(proc == _c->rank);   // no task pushing
  UNUSED(proc);
  TC_START_HTIMER(tc, add);

  task->created_by = _c->rank;
  laws_shrb_push_head(tc->shared_rb, _c->rank, task, sizeof(task_t) + gtc_task_body_size(task));
  ++tc->ct.tasks_spawned;
  TC_STOP_HTIMER(tc, add);
  GTC_EXIT(0);
}


task_t *gtc_task_inplace_create_and_add_laws(gtc_t gtc, task_class_t tclass) {
  GTC_ENTRY();
  tc_t   *tc = gtc_lookup(gtc);
  task_t *t;
  TC_START_HTIMER(tc, addinplace);

  t = (task_t *)laws_shrb_alloc_head(tc->shared_rb);
  gtc_task_set_class(t, tclass);
  t->created_by = _c->rank;
  t->priority   = 0;
  ++tc->ct.tasks_spawned;

  TC_STOP_HTIMER(tc, addinplace);
  GTC_EXIT(t);
}


void gtc_task_inplace_create_and_add_finish_laws(gtc_t gtc, task_t *t) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  UNUSED(t);
  TC_START_HTIMER(tc, addfinish);
  gtc_progress_laws(gtc);   // can't release until the in-place op completes
  TC_STOP_HTIMER(tc, addfinish);
  GTC_EXIT();
}


void gtc_print_stats_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  laws_shrb_t *rb = tc->shared_rb;

  if (!getenv("SCIOTO_DISABLE_STATS") && !getenv("SCIOTO_DISABLE_PERNODE_STATS")) {
    printf(" %4d - laws-Q: nrelease %6lu, nreacquire %6lu (near %lu), refill %lu, resplit %lu, collapse %lu, nprogress %6lu\n"
           " %4d -    near ok %lu fail %lu tasks %lu | far mate ok %lu fail %lu | far rem ok %lu fail %lu | hwm %lu of %d\n",
           _c->rank, rb->nrelease, rb->nreacquire, rb->ct[LCNearReacq], rb->ct[LCRefill], rb->ct[LCResplit],
           rb->ct[LCCollapse], rb->nprogress,
           _c->rank, rb->ct[LCNearOk], rb->ct[LCNearFail], rb->ct[LCNearTasks], rb->ct[LCFarMateOk],
           rb->ct[LCFarMateFail], rb->ct[LCFarRemOk], rb->ct[LCFarRemFail], rb->ct[LCHwm], rb->max_size);
  }
  GTC_EXIT();
}


/** Global stats: the SAWS queue's lines (so runs/e0/parse.py reads both queues alike), then one
 *  line of two-door counters (whole-run totals; hwm is the maximum over PEs). */
void gtc_print_gstats_laws(gtc_t gtc) {
  GTC_ENTRY();
  tc_t *tc = gtc_lookup(gtc);
  laws_shrb_t *rb = (laws_shrb_t *)tc->shared_rb;

  enum { C_GETS, C_STEALS, C_FL, C_FU, C_FA, C_PROG, C_RECL, C_ENS, C_REACQ, C_REL, NC };
  enum { T_POP, T_PERPOP, T_PROG, T_PERPROG, T_RECL, T_PERRECL, T_ENS, T_PERENS, T_REACQ, T_PERREACQ,
         T_REL, T_PERREL, NT };
  double   *t  = gtc_shmem_calloc(NT, sizeof(double)),   *tmin = gtc_shmem_calloc(NT, sizeof(double));
  double   *tmax = gtc_shmem_calloc(NT, sizeof(double)), *tsum = gtc_shmem_calloc(NT, sizeof(double));
  uint64_t *c  = gtc_shmem_calloc(NC, sizeof(uint64_t)), *cmin = gtc_shmem_calloc(NC, sizeof(uint64_t));
  uint64_t *cmax = gtc_shmem_calloc(NC, sizeof(uint64_t)), *csum = gtc_shmem_calloc(NC, sizeof(uint64_t));
  uint64_t *lc = gtc_shmem_calloc(LAWS_NCT, sizeof(uint64_t)), *ls = gtc_shmem_calloc(LAWS_NCT, sizeof(uint64_t));
  uint64_t *lm = gtc_shmem_calloc(LAWS_NCT, sizeof(uint64_t));

  t[T_POP]      = TC_READ_TIMER_MSEC(tc, poptail);
  t[T_PERPOP]   = rb->nsteals    ? TC_READ_TIMER_MSEC(tc, poptail)   / rb->nsteals    : 0.0;
  t[T_PROG]     = TC_READ_TIMER_USEC(tc, progress);
  t[T_PERPROG]  = rb->nprogress  ? TC_READ_TIMER_USEC(tc, progress)  / rb->nprogress  : 0.0;
  t[T_RECL]     = TC_READ_TIMER_USEC(tc, reclaim);
  t[T_PERRECL]  = rb->nreccalls  ? TC_READ_TIMER_USEC(tc, reclaim)   / rb->nreccalls  : 0.0;
  t[T_ENS]      = TC_READ_TIMER_USEC(tc, ensure);
  t[T_PERENS]   = rb->nensure    ? TC_READ_TIMER_USEC(tc, ensure)    / rb->nensure    : 0.0;
  t[T_REACQ]    = TC_READ_TIMER_MSEC(tc, reacquire);
  t[T_PERREACQ] = rb->nreacquire ? TC_READ_TIMER_MSEC(tc, reacquire) / rb->nreacquire : 0.0;
  t[T_REL]      = TC_READ_TIMER_USEC(tc, release);
  t[T_PERREL]   = rb->nrelease   ? TC_READ_TIMER_USEC(tc, release)   / rb->nrelease   : 0.0;
  c[C_GETS]  = rb->ngets;             c[C_STEALS] = rb->nsteals;
  c[C_FL]    = tc->ct.failed_steals_locked;
  c[C_FU]    = tc->ct.failed_steals_unlocked;
  c[C_FA]    = tc->ct.aborted_steals;
  c[C_PROG]  = rb->nprogress;         c[C_RECL]   = rb->nreccalls;
  c[C_ENS]   = rb->nensure;           c[C_REACQ]  = rb->nreacquire;
  c[C_REL]   = rb->nrelease;
  for (int i = 0; i < LAWS_NCT; i++)
    lc[i] = rb->ct[i];

  shmem_barrier_all();
  shmem_double_min_reduce(SHMEM_TEAM_WORLD, tmin, t, NT);
  shmem_double_max_reduce(SHMEM_TEAM_WORLD, tmax, t, NT);
  shmem_double_sum_reduce(SHMEM_TEAM_WORLD, tsum, t, NT);
  shmem_uint64_min_reduce(SHMEM_TEAM_WORLD, cmin, c, NC);
  shmem_uint64_max_reduce(SHMEM_TEAM_WORLD, cmax, c, NC);
  shmem_uint64_sum_reduce(SHMEM_TEAM_WORLD, csum, c, NC);
  shmem_uint64_sum_reduce(SHMEM_TEAM_WORLD, ls, lc, LAWS_NCT);
  shmem_uint64_max_reduce(SHMEM_TEAM_WORLD, lm, lc, LAWS_NCT);
  shmem_barrier_all();

  int n = _c->size;
  eprintf("        : shared heap memory allocated: %d    local heap memory allocated: %d\n", _c->shmallocsize, _c->allocsize);
  eprintf("        : gets         %6lu (%6.2f/%3lu/%3lu) time %6.2fms/%6.2fms/%6.2fms per %6.2fms/%6.2fms/%6.2fms\n",
      csum[C_GETS], csum[C_GETS]/(double)n, cmin[C_GETS], cmax[C_GETS],
      tsum[T_POP]/n, tmin[T_POP], tmax[T_POP], tsum[T_PERPOP]/n, tmin[T_PERPOP], tmax[T_PERPOP]);
  eprintf("        :   steals     %6lu (%6.2f/%3lu/%3lu)\n",
      csum[C_STEALS], csum[C_STEALS]/(double)n, cmin[C_STEALS], cmax[C_STEALS]);
  eprintf("        :   fails lock %6lu (%6.2f/%3lu/%3lu)\n", csum[C_FL], csum[C_FL]/(double)n, cmin[C_FL], cmax[C_FL]);
  eprintf("        :   fails un   %6lu (%6.2f/%3lu/%3lu)\n", csum[C_FU], csum[C_FU]/(double)n, cmin[C_FU], cmax[C_FU]);
  eprintf("        :   fails ab   %6lu (%6.2f/%3lu/%3lu)\n", csum[C_FA], csum[C_FA]/(double)n, cmin[C_FA], cmax[C_FA]);
  eprintf("        : progress   %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus per %6.2fus/%6.2fus/%6.2fus\n",
      csum[C_PROG]/(double)n, cmin[C_PROG], cmax[C_PROG],
      tsum[T_PROG]/n, tmin[T_PROG], tmax[T_PROG], tsum[T_PERPROG]/n, tmin[T_PERPROG], tmax[T_PERPROG]);
  eprintf("        : reclaim    %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus per %6.2fus/%6.2fus/%6.2fus\n",
      csum[C_RECL]/(double)n, cmin[C_RECL], cmax[C_RECL],
      tsum[T_RECL]/n, tmin[T_RECL], tmax[T_RECL], tsum[T_PERRECL]/n, tmin[T_PERRECL], tmax[T_PERRECL]);
  eprintf("        : ensure     %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus per %6.2fus/%6.2fus/%6.2fus\n",
      csum[C_ENS]/(double)n, cmin[C_ENS], cmax[C_ENS],
      tsum[T_ENS]/n, tmin[T_ENS], tmax[T_ENS], tsum[T_PERENS]/n, tmin[T_PERENS], tmax[T_PERENS]);
  eprintf("        : reacquire  %6.2f/%3lu/%3lu time %6.2fms/%6.2fms/%6.2fms per %6.2fms/%6.2fms/%6.2fms\n",
      csum[C_REACQ]/(double)n, cmin[C_REACQ], cmax[C_REACQ],
      tsum[T_REACQ]/n, tmin[T_REACQ], tmax[T_REACQ], tsum[T_PERREACQ]/n, tmin[T_PERREACQ], tmax[T_PERREACQ]);
  eprintf("        : release    %6.2f/%3lu/%3lu time %6.2fus/%6.2fus/%6.2fus per %6.2fus/%6.2fus/%6.2fus\n",
      csum[C_REL]/(double)n, cmin[C_REL], cmax[C_REL],
      tsum[T_REL]/n, tmin[T_REL], tmax[T_REL], tsum[T_PERREL]/n, tmin[T_PERREL], tmax[T_PERREL]);

  // two-door counters: per door claims, owner operations, the realized export share
  {
    double near_us = ls[LCNearOk] ? (ls[LCNearTsc] / _c->tsc_cpu_hz) / ls[LCNearOk] : 0.0;  // tsc_cpu_hz is MHz
    const char *doors = tc->ldbal_cfg.laws_doors == LAWS_DOORS_ANY ? "any"
                      : tc->ldbal_cfg.laws_doors == LAWS_DOORS_NEAR_ONLY ? "near-only" : "near-first";
    if (rb->far_sync) eprintf("        : laws2 far sync on (shmem_quiet after each far publish)\n");
    eprintf("        : laws2 rho %.3f doors %s refill %d resplit %d near %s | release %lu far %lu near %lu"
            " | refill %lu tasks %lu skip space %lu epoch %lu | resplit %lu tasks %lu | collapse %lu"
            " | reacq near %lu tasks %lu far %lu tasks %lu | maint %lu waited %lu"
            " | near ok %lu tasks %lu fail %lu peekmiss %lu avg %.3f us | farmate ok %lu tasks %lu fail %lu"
            " | farrem ok %lu tasks %lu fail %lu | hwm %lu of %d\n",
        rb->rho_pm / 1000.0, doors, rb->refill, rb->resplit, rb->near_on ? "on" : "off",
        ls[LCRelease], ls[LCRelFar], ls[LCRelNear],
        ls[LCRefill], ls[LCRefillTasks], ls[LCRefillSpace], ls[LCRefillEpoch],
        ls[LCResplit], ls[LCResplitTasks], ls[LCCollapse],
        ls[LCNearReacq], ls[LCNearReacqTasks], ls[LCFarReacq], ls[LCFarReacqTasks],
        ls[LCMaint], ls[LCWaited],
        ls[LCNearOk], ls[LCNearTasks], ls[LCNearFail], ls[LCNearPeekMiss], near_us,
        ls[LCFarMateOk], ls[LCFarMateTasks], ls[LCFarMateFail],
        ls[LCFarRemOk], ls[LCFarRemTasks], ls[LCFarRemFail],
        lm[LCHwm], rb->max_size);
  }

  eprintf("&&&  %6.2f %6.2f ", tsum[T_POP]/n, tsum[T_REACQ]/n);

  shmem_free(t); shmem_free(tmin); shmem_free(tmax); shmem_free(tsum);
  shmem_free(c); shmem_free(cmin); shmem_free(cmax); shmem_free(csum);
  shmem_free(lc); shmem_free(ls); shmem_free(lm);
  GTC_EXIT();
}
