#include <stdio.h>
#include <unistd.h>

#include <shr_ring.h>

#define NUM 1000

int main(int argc, char **argv, char **envp) {
    int errors = 0;
    int i;
    shrb_t *rb;
    int x;
    int digits[NUM];

    // just need this to setup Portals world
    gtc_init();

    rb = shrb_create(sizeof(int), 100000);

    if (rb->procid == 0) printf("\nPortals Shared ring buffer test: Started with %d threads\n\n", rb->nproc);
    if (rb->procid == 0) printf("TEST: everyone push_head() onto thread 0\n");

    // Push everything onto processor 0
    for (i = 0; i < NUM; i++) {
        //shrb_lock(rb, 0);
        shrb_push_head(rb, 0, &i, sizeof(int));
        //shrb_unlock(rb, 0);
    }

    gtc_barrier();

    for (i = 0; i < NUM; i++) digits[i] = 0;

    while (rb->procid == 0 && shrb_pop_head(rb, 0, &x)) {
        //printf(" -- %d: Got digit %d\n", rb->procid, x);
        digits[x]++;
    }

    gtc_barrier();

    for (i = 0; rb->procid == 0 && i < NUM; i++) {
        if (digits[i] != rb->nproc) {
            printf(" -- %d: Error, count for %d is %d, expected %d\n", rb->procid, i, digits[i], rb->nproc);
            errors++;
        }
    }

    shrb_destroy(rb);

    if (rb->procid == 0) printf("Test finished: %d errors.\n", errors);
    return 0;
}
