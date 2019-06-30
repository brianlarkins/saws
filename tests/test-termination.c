#include <stdio.h>
#include <stdlib.h>
#include <tc.h>

int main(int argc, char **argv) {
  int i;
  td_t *td;
  
  gtc_init();
  td = td_create();

  if (_c->rank == 0)
    printf("Termination detection tree test starting with %d threads\n", _c->size);

  fflush(stdout);
  gtc_barrier();

  for (i = 0; !td_attempt_vote(td); i++) {
    if (rand() > ((i/100 < 32) ? 2>>(i/100) : RAND_MAX))
        td->token.state = ACTIVE;
  }
  gtc_barrier();
  
  if (_c->rank == 0)
    printf("Termination: SUCCESS\n");

  td_destroy(td);
  
  return 0;
}
