#include <stdio.h>
#include <unistd.h>

#include <tc.h>
#include <shr_ring.h>

#define NUM   817
#define QSIZE 1000
#define NITER 10

#define TARGET (rb->procid+1)%rb->nproc /* round robin */
//#define TARGET rb->procid /* local only */


void print_queue(shrb_t *rb) {
  int *iap = (int *)rb->q;

  shrb_print(rb);

  for (int i=0; i<QSIZE; i++) {
    printf("%d: q[%d] = %d\n", _c->rank, i, iap[i]);
  }
}

int main(int argc, char **argv, char **envp) {
  int errors = 0;
  int i, iter;
  shrb_t *rb;
  int x;
  int cnt;

  // just need this to setup Portals world
  gtc_init();

  rb = shrb_create(sizeof(int), QSIZE);

  if (rb->procid == 0) printf("\nPortals Shared ring buffer test: Started with %d threads\n\n", rb->nproc);

  for (iter = 0; iter < NITER; iter++) {
    if (rb->procid == 0) printf("\n-- Iteration %d, errors %d\n\n", iter, errors);
    if (rb->procid == 0) printf("TEST: push_head() -> pop_head()\n");

    for (i = 1; i <= NUM; i++)
      shrb_push_head(rb, TARGET, &i, sizeof(int));

    gtc_barrier();

    for (i=NUM,cnt=0; shrb_pop_head(rb, TARGET, &x) > 0; i--,cnt++) {
      if (x != i) {
        printf(" -- %d: Error, got %d expected %d\n", rb->procid, x, i);
        ++errors;
      }
    }

    if (cnt < NUM) {
      printf(" -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
      ++errors;
    }

    gtc_barrier();

    if (rb->procid == 0) printf("TEST: push_tail() -> pop_tail()\n");

    for (i = 1; i <= NUM; i++) {
      shrb_push_tail(rb, TARGET, &i, sizeof(int));
    }

    gtc_barrier();

    for (i=NUM,cnt=0; shrb_pop_tail(rb, TARGET, &x) > 0; i--,cnt++) {
      if (x != i) {
        printf(" -- %d: Error, got %d expected %d\n", rb->procid, x, i);
        ++errors;
      }
    }

    gtc_barrier();

    if (cnt < NUM) {
      printf(" -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
      ++errors;
    }

    gtc_barrier();
    if (rb->procid == 0) printf("TEST: push_n_head() -> pop_tail()\n");

    int y[NUM];
    for (i = 0; i < NUM; i++)
      y[i] = (NUM-i)+1000;

    shrb_push_n_head(rb, TARGET, y, NUM);

    gtc_barrier();

    for (i=NUM; shrb_pop_tail(rb, TARGET, &x); i--) {
      if (x != (i+1000)) {
        printf(" -- %d: Error, got %d expected %d\n", rb->procid, x, i);
        ++errors;
      }
    }

    gtc_barrier();
    if (rb->procid == 0) printf("TEST: push_tail() -> pop_n_tail()\n");
    int e[NUM];
    for (i = 1; i <= NUM; i++) {
      x = -i;
      shrb_push_tail(rb, TARGET, &x, sizeof(int));
    }

    gtc_barrier();
    shrb_pop_n_tail(rb, TARGET, NUM, e, STEAL_ALL);

    for (i = 0; i < NUM; i++) {
      if (e[i] != -(NUM-i)) {
        printf(" +- %d: Error, e[%d] =  %d expected %d\n", rb->procid, i, e[i], -(NUM-i));
        ++errors;
      }
    }

    gtc_barrier(); 
  }
  

  shrb_destroy(rb);

  if (rb->procid == 0) printf("\nTest finished: %d errors.\n", errors);
  fflush(stdout);

  if (errors) return 1;
  return 0;
}
