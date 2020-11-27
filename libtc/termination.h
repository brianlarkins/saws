#ifndef __TERMINATION_H__
#define __TERMINATION_H__
#include <inttypes.h>

#define TERMINATION_TAG 1

enum token_states { ACTIVE, TERMINATED };
enum token_directions { UP, DOWN };

typedef struct {
  int  state;
  int  spawned;   // counter1
  int  completed; // counter2
} td_token_t;

struct td_s {
  int procid, nproc;
  int p;                  // parent rank
  int l;                  // left child rank
  int r;                  // right child rank
  int nchildren;          // number of children
  int num_cycles;
  int num_attempts;       // number of failed termination attempts
  int have_voted;
  enum token_directions token_direction;

  td_token_t token;
  td_token_t down_token;
  td_token_t upleft_token;
  td_token_t upright_token;
  td_token_t send_token;

  uint64_t   left_voted;  // signal counters for OpenSHMEM signal puts
  uint64_t   right_voted;
  uint64_t   parent_voted;

  uint64_t   last_left;   // last observed signal values
  uint64_t   last_right;
  uint64_t   last_parent;

  int last_spawned;
  int last_completed;

};
typedef struct td_s td_t;

td_t *td_create();
void  td_destroy(td_t *td);
void  td_reset(td_t *td);

int   td_attempt_vote(td_t *td);
void  td_set_counters(td_t *td, int count1, int count2);
int   td_get_counter1(td_t *td);
int   td_get_counter2(td_t *td);

#endif /* __TERMINATION_H__ */
