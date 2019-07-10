#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <shmem.h>

// #include "debug.h"
#include "termination.h"

// Comment this out to use blocking sends when passing the token down
#define TD_NONBLOCKING

/** Static function prototypes **/

static void token_reset(td_token_t *t);

static int  get_parent_id(td_t *td);
static int  get_left_child_id(td_t *td);
static int  get_right_child_id(td_t *td);
static int  have_children(td_t *td);

static void pass_token_up(td_t *td, td_token_t *token);
static void pass_token_down(td_t *td, td_token_t *token);


/** Token Manipulation Functions **/

/** Update the values of the two counters <tasks created, tasks executed>.
  *
  * @param[in] td     Termination detection context.
  * @param[in] count1 New value for counter 1.
  * @param[in] count2 New value for counter 2.
  */
void td_set_counters(td_t *td, int count1, int count2) {
  td->my_token.counter1 = count1;
  td->my_token.counter2 = count2;
}

/** Read counter 1.
  */
int td_get_counter1(td_t *td) {
  return td->my_token.counter1;
}

/** Read counter 2.
  */
int td_get_counter2(td_t *td) {
  return td->my_token.counter2;
}

static void token_reset(td_token_t *t) {
  t->state    = ACTIVE;
  t->counter1 = 0;
  t->counter2 = 0;
}


/** Functions to help us find our place in the tree **/

static int get_parent_id(td_t *td) {
  if (td->procid == 0)
    return -1;
  else
    return (td->procid - 1) / 2;
}

static int get_left_child_id(td_t *td) {
  int child = 2*td->procid + 1;

  return (child >= td->nproc) ? -1 : child;
} 

static int get_right_child_id(td_t *td) {
  int child = 2*td->procid + 2;

  return (child >= td->nproc) ? -1 : child;
} 

static int have_children(td_t *td) {
  return get_left_child_id(td) >= 0 || get_right_child_id(td) >= 0 ? 1 : 0;
}


/** Communication Functions -- Move the token up and down the tree **/

static void pass_token_up(td_t *td, td_token_t *token) {
  if (td->procid > 0) {
    if (get_parent_id(td) * 2 + 1 == td->procid) { // Left child
      shmem_putmem(&td->left_child_token, token, sizeof(td_token_t), get_parent_id(td));
    }
    else {                                     // Right child
      shmem_putmem(&td->right_child_token, token, sizeof(td_token_t), get_parent_id(td));
    }
  }
}

static void pass_token_down(td_t *td, td_token_t *token) {
#ifdef TD_NONBLOCKING
 
  // It should be faster to send both messages concurrently:
  if (get_left_child_id(td) >= 0)
    shmem_putmem_nbi(&td->parent_token, token, sizeof(td_token_t), get_left_child_id(td));

  if (get_right_child_id(td) >= 0)
    shmem_putmem_nbi(&td->parent_token, token, sizeof(td_token_t), get_right_child_id(td));

  shmem_quiet();
#else
  if (get_left_child_id(td) >= 0)
    shmem_putmem(&td->parent_token, token, sizeof(td_token_t), get_left_child_id(td));

  if (get_right_child_id(td) >= 0)
    shmem_putmem(&td->parent_token, token, sizeof(td_token_t), get_right_child_id(td));

#endif /* TD_NONBLOCKING */
}

#undef TD_NONBLOCKING


/** Reset a termination detection context so that it can be re-used.
  *
  * @param[in] td Termination detection context.
  */
void td_reset(td_t *td) {
  // MPI_Barrier(td->comm);
  shmem_barrier_all();
  
  token_reset(&td->my_token);
  token_reset(&td->left_child_token);
  token_reset(&td->right_child_token);
  token_reset(&td->parent_token);

  td->num_cycles = 0;
  td->token_direction = UP;
  td->have_voted = 0;

  // cancel_parent_listener(td);                      // Call to function that needs to be fixed
  // cancel_child_listeners(td);

  // Initially, give phantom "tokens" to all the leaf nodes
  if (!have_children(td)) {
    // td->parent_req = MPI_REQUEST_NULL;
    td->token_direction = DOWN;
  } 

  td->last_counter1 = 0;
  td->last_counter2 = 0;
}


/** Create a termination detection context.
  *
  * @return         Termination detection context.
  */
td_t *td_create() {
  td_t *td = shmem_malloc(sizeof(td_t));

  assert(td != NULL);
  
  td->nproc = shmem_n_pes();
  td->procid = shmem_my_pe();

  td_reset(td);

#if 0
  DEBUG(DBGTD,
    printf("TD Created (%d of %d): parent=%d, left_child=%d, right_child=%d, direction=%s\n",
      td->procid, td->nproc, get_parent_id(td), get_left_child_id(td), get_right_child_id(td),
      td->token_direction == UP ? "UP" : "DOWN")
  );
#endif 
  return td;
}


/** Free the termination detection context.
  *
  * @param[in] td Termination detection context.
  */
void td_destroy(td_t *td) {
  // DEBUG(DBGTD, printf("Destroying TD (%d of %d)\n", td->procid, td->nproc));
  shmem_free(td);
}


/** Attempt to detect termination detection.
  *
  * @param[in] td Termination detection context.
  * @return       Non-zero upon termination, zero othersize.
  */
int td_attempt_vote(td_t *td) {
  // Special Case: 1 Thread
  if (td->nproc == 1) {
    if (   td->my_token.counter1 == td->last_counter1 
        && td->my_token.counter2 == td->last_counter2
        && td->my_token.counter1 == td->my_token.counter2)
      td->my_token.state = TERMINATED;

    td->last_counter1 = td->my_token.counter1;
    td->last_counter2 = td->my_token.counter2;

  // Case 1: Token is moving down the tree
  } 
  else if (td->token_direction == DOWN) {
    int        flag = 1;
 
    // MPI_Test(&td->parent_req, &flag, &status);
    // shmem_test(&flag, );                               // Fix - shmem_test()

    if (flag == 1) {
      // CASE 1.1: Leaf Node
      if (!have_children(td)) {
        if (td->parent_token.state == TERMINATED) {
          td->my_token.state = TERMINATED;
        } 
        else {
          // Leaves reverse token direction and cast the first votes
          pass_token_up(td, &td->my_token);
          td->token_direction = DOWN;
          td->have_voted      = 1;
        }

      // CASE 1.2: Interior Node
      } 
      else {
        if (td->parent_token.state == TERMINATED) {
          td->my_token.state = TERMINATED;
        }
        pass_token_down(td, &td->parent_token);
        td->have_voted      = 0;
        td->token_direction = UP;
      }

      // if (td->parent_token.state != TERMINATED) listen_parent(td);
    }
   
  // Case 2: Token is moving up the tree  
  } 
  else {
    int        flag_left = 1;
    int        flag_right = 1;
    td_token_t new_token;

    token_reset(&new_token);
  
    // MPI_Test(&td->left_child_req, &flag_left, &status_left);         // Fix - shmem_test()
    // MPI_Test(&td->right_child_req, &flag_right, &status_right);
    
    if (flag_left == 1 && flag_right == 1) {
      // CASE 2.1: Root Node
      if (td->procid == 0) {
        int counter1 = td->my_token.counter1 + td->left_child_token.counter1 + td->right_child_token.counter1;
        int counter2 = td->my_token.counter2 + td->left_child_token.counter2 + td->right_child_token.counter2;

        if (counter1 == td->last_counter1 && counter2 == td->last_counter2 && counter1 == counter2)
          td->my_token.state = TERMINATED;

        td->last_counter1 = counter1;
        td->last_counter2 = counter2;

        td->num_cycles++;
        new_token.state    = td->my_token.state;
        new_token.counter1 = td->my_token.counter1 + td->left_child_token.counter1 + td->right_child_token.counter1;
        new_token.counter2 = td->my_token.counter2 + td->left_child_token.counter2 + td->right_child_token.counter2;
#if 0       
	DEBUG(DBGTD, 
          if (new_token.state == TERMINATED)
            printf("Termination after %d cycles, counter1=%d counter2=%d\n", td->num_cycles, new_token.counter1, new_token.counter2);
        );
#endif
        pass_token_down(td, &new_token);
        td->token_direction = UP;
        td->have_voted      = 0;

      // CASE 2.2: Interior Node -- merge tokens and pass the result on to my parent
      } 
      else {
        new_token.counter1 = td->my_token.counter1 + td->left_child_token.counter1 + td->right_child_token.counter1;
        new_token.counter2 = td->my_token.counter2 + td->left_child_token.counter2 + td->right_child_token.counter2;

        pass_token_up(td, &new_token);
        td->token_direction = DOWN;
        td->have_voted      = 1;
      }

      /* if (td->my_token.state != TERMINATED) {
        listen_left_child(td);
        listen_right_child(td);
      } */
    }
  }

  // DEBUG(DBGTD, if (td->my_token.state == TERMINATED) printf("  Thread %d: Detected Termination\n", td->procid));
  
  return td->my_token.state == TERMINATED ? 1 : 0;
}
