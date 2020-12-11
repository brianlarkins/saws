#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <shmem.h>

// #include "debug.h"
#include "tc.h"
#include "termination.h"

// Comment this out to use blocking sends when passing the token down
#define TD_NONBLOCKING

/** Static function prototypes **/

static void token_reset(td_token_t *t);

static void pass_token_up(td_t *td);
static void pass_token_down(td_t *td);


/** Token Manipulation Functions **/

/** Update the values of the two counters <tasks created, tasks executed>.
  *
  * @param[in] td     Termination detection context.
  * @param[in] count1 New value for counter 1.
  * @param[in] count2 New value for counter 2.
  */
void td_set_counters(td_t *td, int count1, int count2) {
  GTC_ENTRY();
  td->token.spawned = count1;
  td->token.completed = count2;
  GTC_EXIT();
}


static void token_reset(td_token_t *t) {
  GTC_ENTRY();
  t->state    = ACTIVE;
  t->spawned = 0;
  t->completed = 0;
  GTC_EXIT();
}

/** Communication Functions -- Move the token up and down the tree **/

/**
 * pass_token_down - broadcast global termination status
 * @param td termination detection state
 */
static void pass_token_down(td_t *td) {
  GTC_ENTRY();
  __gtc_marker[2] = 1;
  gtc_lprintf(DBGTD, "td: passing token down: send_token: [ %s %d %d ] last : s: %d c: %d nkids: %d l: %d r: %d\n",
      td->send_token.state == ACTIVE ? "a" : "t", td->send_token.spawned, td->send_token.completed,
      td->last_spawned, td->last_completed, td->nchildren, td->l, td->r);

  if (td->nchildren > 0) {
#if GTC_USE_SIGNAL_COMMS
    shmem_putmem_signal_nbi(&td->down_token, &td->send_token, sizeof(td_token_t), &td->parent_voted, 1, SHMEM_SIGNAL_ADD, td->l);
#else
    shmem_putmem(&td->down_token, &td->send_token, sizeof(td_token_t), td->l);
    shmem_atomic_inc(&td->parent_voted, td->l);
#endif // GTC_USE_SIGNAL_COMMS

    if (td->nchildren == 2) {
#if GTC_USE_SIGNAL_COMMS
      shmem_putmem_signal_nbi(&td->down_token, &td->send_token, sizeof(td_token_t), &td->parent_voted, 1, SHMEM_SIGNAL_ADD, td->r);
#else
      shmem_putmem(&td->down_token, &td->send_token, sizeof(td_token_t), td->r);
      shmem_atomic_inc(&td->parent_voted, td->r);
#endif // GTC_USE_SIGNAL_COMMS
    }
  }
  shmem_quiet();
  __gtc_marker[2] = 0;
  td->num_cycles++;
  GTC_EXIT();
}



/**
 * pass_token_up - reduce global termination status
 * @param td termination detection state
 */
static void pass_token_up(td_t *td) {
  GTC_ENTRY();
  __gtc_marker[2] = 2;
  gtc_lprintf(DBGTD, "td: passing token up: send_token: [ %s %d %d ] last : s: %d c: %d\n",
      td->send_token.state == ACTIVE ? "a" : "t", td->send_token.spawned, td->send_token.completed,
      td->last_spawned, td->last_completed);

  if ((td->procid % 2) == 1) {
#if GTC_USE_SIGNAL_COMMS
      shmem_putmem_signal_nbi(&td->upleft_token, &td->send_token, sizeof(td_token_t), &td->left_voted, 1, SHMEM_SIGNAL_ADD, td->p);
#else
      shmem_putmem(&td->upleft_token, &td->send_token, sizeof(td_token_t), td->p);
      shmem_atomic_inc(&td->left_voted, td->p);
#endif // GTC_USE_SIGNAL_COMMS
  } else {
#if GTC_USE_SIGNAL_COMMS
      shmem_putmem_signal_nbi(&td->upright_token, &td->send_token, sizeof(td_token_t), &td->right_voted, 1, SHMEM_SIGNAL_ADD, td->p);
#else
      shmem_putmem(&td->upright_token, &td->send_token, sizeof(td_token_t), td->p);
      shmem_atomic_inc(&td->right_voted, td->p);
#endif // GTC_USE_SIGNAL_COMMS
  }
  shmem_quiet();
  __gtc_marker[2] = 0;
  GTC_EXIT();
}



/** Create a termination detection context.
  *
  * @return         Termination detection context.
  */
td_t *td_create() {
  GTC_ENTRY();
  td_t *td = shmem_malloc(sizeof(td_t));

  assert(td != NULL);

  td->nproc = shmem_n_pes();
  td->procid = shmem_my_pe();

  td->p = ((td->procid + 1) >> 1) - 1;
  td->l = ((td->procid + 1) << 1) - 1;
  td->r = td->l + 1;

  td->nchildren = 0;
  if (td->l < td->nproc) td->nchildren++;
  if (td->r < td->nproc) td->nchildren++;

  td_reset(td);

  gtc_lprintf(DBGTD,"TD Created (%d of %d): parent=%d, left_child=%d, right_child=%d, direction=%s\n",
      td->procid, td->nproc, td->p, td->l, td->r, td->token_direction == UP ? "UP" : "DOWN");

  GTC_EXIT(td);
}



/** Reset a termination detection context so that it can be re-used.
  *
  * @param[in] td Termination detection context.
  */
void td_reset(td_t *td) {
  GTC_ENTRY();
  shmem_barrier_all();

  token_reset(&td->token);
  token_reset(&td->upleft_token);
  token_reset(&td->upright_token);
  token_reset(&td->down_token);
  token_reset(&td->send_token);

  td->num_cycles   = 0;
  td->num_attempts = 0;

  td->parent_voted = 0;
  td->left_voted   = 0;
  td->right_voted  = 0;

  td->last_spawned = 0;
  td->last_completed = 0;

  td->token_direction = UP;

  shmem_barrier_all();
  GTC_EXIT();
}



/** Free the termination detection context.
  *
  * @param[in] td Termination detection context.
  */
void td_destroy(td_t *td) {
  GTC_ENTRY();
  gtc_lprintf(DBGTD, "Destroying TD (%d of %d)\n", td->procid, td->nproc);
  shmem_free(td);
  GTC_EXIT();
}


/** Attempt to detect termination detection.
  *
  * @param[in] td Termination detection context.
  * @return       Non-zero upon termination, zero othersize.
  */
int td_attempt_vote(td_t *td) {
  GTC_ENTRY();
  uint64_t nleft, nright, ndown;
  int have_votes;
  __gtc_marker[2] = 4;
  // Special Case: 1 Thread
  if (td->nproc == 1) {
    if (   td->token.spawned == td->last_spawned
        && td->token.completed == td->last_completed
        && td->token.spawned == td->token.completed) {
      td->token.state = TERMINATED;
      gtc_lprintf(DBGTD, "td_attempt_vote: thread detected termination\n");
    }

    td->last_spawned = td->token.spawned;
    td->last_completed = td->token.completed;
    return td->token.state == TERMINATED ? 1 : 0;
  }
#if GTC_USE_SIGNAL_COMMS
  __gtc_marker[3] = td->l;
  nleft  = shmem_signal_fetch(&td->left_voted);
  __gtc_marker[3] = td->r;
  nright = shmem_signal_fetch(&td->right_voted);
  ndown  = shmem_signal_fetch(&td->parent_voted);
#else
  nleft  = shmem_atomic_fetch(&td->left_voted, td->procid);
  nright = shmem_atomic_fetch(&td->right_voted, td->procid);
  ndown  = shmem_atomic_fetch(&td->parent_voted, td->procid);
#endif // GTC_USE_SIGNAL_COMMS
  shmem_quiet();
  __gtc_marker[2] = 5;
  gtc_lprintf(DBGTD, "td_attempt_vote: %s nl: %d nr: %d nd: %d last-l: %d last-r: %d last-p: %d\n",
      td->token_direction == UP ? "UP" : "DOWN", nleft, nright, ndown,
      td->last_left, td->last_right, td->last_parent);

  // Case 1: Token is moving down the tree
  if (td->token_direction == DOWN) {
    // have we received a vote from the parent?
    if ((td->procid == 0) || (ndown > td->last_parent)) {
      if (td->nchildren == 0) {
        // leaf
        if (td->down_token.state == TERMINATED) {
          td->token.state = TERMINATED;
        } else {
          // restart reduction
          gtc_lprintf(DBGTD, "td_attempt_vote: restarting vote\n");
          td->send_token = td->token;
          pass_token_up(td);
          td->last_parent = ndown;
        }
      } else {
        // interior node
        gtc_lprintf(DBGTD, "td_attempt_vote: casting downward votes\n");
        if (td->down_token.state == TERMINATED)
          td->token.state = TERMINATED;
        td->send_token = td->down_token; // struct copy
        pass_token_down(td);
        td->token_direction = UP;

        if (td->down_token.state != TERMINATED) {
          // expect another vote from parent
          td->last_parent = ndown;
        }
      }
    }

  } else {
    // if we've received votes from left and right:
    have_votes = 0;
    switch (td->nchildren) {
      case 0:
        have_votes = 1;
        break;
      case 1:
        if (nleft > td->last_left)
          have_votes = 1;
        break;
      case 2:
        if ((nleft > td->last_left) && (nright > td->last_right))
          have_votes = 1;
        break;
    }

    if (have_votes) {
      int spawned   = td->token.spawned   + td->upleft_token.spawned   + td->upright_token.spawned;
      int completed = td->token.completed + td->upleft_token.completed + td->upright_token.completed;

      // if root
      if (td->procid == 0) {

        td->num_attempts++; // track termination attempts

        if (((spawned == td->last_spawned) && (completed == td->last_completed)) &&
              (spawned == completed))
          td->token.state = TERMINATED;

        td->last_spawned   = spawned;
        td->last_completed = completed;

        gtc_lprintf(DBGTD, "td_attempt_vote: broadcasting termination state : token: %d %d ul: %d %d ur: %d %d\n",
            td->token.spawned, td->token.completed,
            td->upleft_token.spawned, td->upleft_token.completed,
            td->upright_token.spawned, td->upright_token.completed);
        td->send_token.state     = td->token.state;
        td->send_token.spawned   = spawned;
        td->send_token.completed = completed;
        pass_token_down(td);
        td->token_direction = UP;

      } else {
        // else if interior or leaf node

        gtc_lprintf(DBGTD, "td_attempt_vote: broadcasting termination state\n");
        td->send_token.state     = td->token.state;
        td->send_token.spawned   = spawned;
        td->send_token.completed = completed;
        pass_token_up(td);
        td->token_direction = DOWN;
      }

      if (td->token.state != TERMINATED) {
        td->last_left  = nleft;
        td->last_right = nright;
      }
    }
  }

  if (td->token.state == TERMINATED) {
    gtc_lprintf(DBGTD, "td_attempt_vote: thread detected termination\n");
  }
  __gtc_marker[2] = 0;
  __gtc_marker[3] = 0;
  GTC_EXIT(td->token.state == TERMINATED ? 1 : 0);
}
