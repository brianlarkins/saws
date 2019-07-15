#ifndef __TERMINATION_H__
#define __TERMINATION_H__

#define TERMINATION_TAG 1

enum token_states { ACTIVE, TERMINATED };
enum token_directions { UP, DOWN };

typedef struct {
  int  state;
  int  valid;
  int  counter1;
  int  counter2;
} td_token_t;

struct td_s {
  int procid, nproc;
  int num_cycles;
  int have_voted;
  enum token_directions token_direction;

  td_token_t my_token;
  td_token_t parent_token;
  td_token_t left_child_token;
  td_token_t right_child_token;
  td_token_t temp_token;

  int last_counter1;
  int last_counter2;
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
