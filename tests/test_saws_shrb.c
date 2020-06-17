#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <shmem.h>
#include <tc.h>
#include <saws_shrb.h>

#define DEBUG 0
#define GTC_TEST_DEBUG   0
#define QSIZE   4
#define NUM     4
#define NUMREPS 1
#define TAILTARGET  (rb->procid + 1) % rb->nproc /* round robin */
#define HEADTARGET  rb->procid               /* local only */

typedef struct {
    int    id;
    char   junk[100];
    int    check;
} elem_t;

void print_queue(saws_shrb_t *rb) {  
    elem_t *ela = (elem_t *) rb->q;   

    saws_shrb_print(rb);  
    for (int i = 0; i < QSIZE; i++)    
        printf("%d: q[%d] = %d :: %d\n", _c->rank, i, ela[i].id, ela[i].check);
}

void print_buf(elem_t *a) {
    for (int i = 0; i < NUM; i++)
        printf("%d: q[%d] = %d :: %d\n", _c->rank, i, a[i].id, a[i].check);			
}

//prints binary representation of an integer
void itob(long v)
{
    unsigned int mask=1<<((sizeof(int)<<3)-1);
    while(mask) {
        printf("%d", (v&mask ? 1 : 0));
        mask >>= 1;
    }
}



int main(int argc, char **argv, char **envp) {

    int i, j, cnt, errors;

    saws_shrb_t *rb;		// Ring buffer

    elem_t *x;
    elem_t y[NUM];

    setbuf(stdout, NULL);

    _c = gtc_init();			// Initialize environment
    shmem_barrier_all();

    rb = saws_shrb_create(sizeof(elem_t), QSIZE);
    x = saws_shrb_malloc(QSIZE, sizeof(elem_t));

    if (rb->procid == 0) 
        printf("\n SAWS ring buffer test: Started with %d threads\n\n", rb->nproc);

    for (int rep = 0; rep < NUMREPS; rep++) {
        errors = 0;
        //if (rb->procid == 0) print_queue(rb);
        if (rb->procid == 0) printf(" TEST: saws_push_head() -> saws_pop_head()\n");

        for (int i = 1; i <= NUM; i++) {
            y[0].id = y[0].check = i;
            saws_shrb_push_head(rb, HEADTARGET, &y[0], sizeof(elem_t));
        }

        shmem_barrier_all();

        for (i = NUM, cnt = 0; saws_shrb_pop_head(rb, HEADTARGET, x) > 0; i--,cnt++) {
            if (x->id != i && x->check != i) {
                printf("  -- %d: Error, got <%d, %d> expected <%d, %d>\n", rb->procid, x->id, x->check, i, i);
                errors++;
            }
        }

        if (cnt < NUM) {
            printf("  -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
            errors++;
        }
        if (errors > 0) printf("*** test failed -- saws_push_head() -> saws_pop_head() ***\n");

        shmem_barrier_all();

        if (DEBUG) saws_shrb_print(rb);
        if (rb->procid == 0) printf(" TEST: push_n_head() -> pop_head()\n");

        for (int i = 0; i < NUM; i++)
            y[i].id = y[i].check = i;

        saws_shrb_push_n_head(rb, HEADTARGET, y, NUM);

        shmem_barrier_all();

        for (i=NUM-1,cnt=0; saws_shrb_pop_head(rb, HEADTARGET, x) > 0; i--,cnt++) {
            if (x->id != i && x->check != i) {
                printf("  -- %d: Error, got <%d, %d> expected <%d, %d>\n", rb->procid, x->id, x->check, i, i);
                errors++;
            }
        }

        if (cnt < NUM) {
            printf("  -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
            errors++;
        }

        shmem_barrier_all();

        if (rb->procid == 0) printf(" TEST: saws_shrd_release() -> saws_shrb_aquire()\n");

        saws_shrb_push_n_head(rb, HEADTARGET, y, NUM);

        saws_shrb_release(rb);
        //if (rb->procid == 0) saws_shrb_print(rb);
        assert(saws_shrb_shared_size(rb) >= saws_shrb_local_size(rb));
        if (saws_shrb_shared_size(rb) < saws_shrb_local_size(rb)) {
            errors++;
            printf("*** test failed -- saws_shrb_release()***\n");
        } 
        if (!(rb->steal_val > 0)) errors++;

        //saws_shrb_release_all(rb);
        //if (saws_shrb_local_size(rb) != 0) errors++;
        int nloc = saws_shrb_local_size(rb);
        printf("nloc %d\n", nloc);
        for(i = 0; i < nloc; i++) saws_shrb_pop_head(rb, HEADTARGET, x);
        assert(saws_shrb_local_size(rb) == 0);
        saws_shrb_reacquire(rb);
        //if (rb->procid == 0) saws_shrb_print(rb);
        if(saws_shrb_shared_size(rb) > saws_shrb_local_size(rb)) {
            errors++;
            printf("*** test failed -- saws_shrb_reaquire()***\n");
        }
        shmem_barrier_all();
        i = saws_shrb_pop_tail(rb, TAILTARGET, x);
        if (rb->procid == 0) printf(" %d tasks was stolen\n", i);
        if (rb->procid == 0) saws_shrb_print(rb);
        shmem_barrier_all();
        printf("calling release\n");
        saws_shrb_release(rb); 
        //if (rb->procid == 0) saws_shrb_print(rb);
        if (rb->procid == 0) saws_shrb_print(rb);
        for (i=NUM-1,cnt=0; saws_shrb_pop_head(rb, HEADTARGET, x) > 0; i--,cnt++) {
        }
        shmem_barrier_all();


        /*********************** important test ************************************/

        if (DEBUG) saws_shrb_print(rb);
        if (rb->procid == 0) printf(" TEST: push_n_head() -> pop_tail()\n");

        for (i = 0; i < NUM; i++)
            y[i].id = y[i].check = NUM-i;
        saws_shrb_push_n_head(rb, HEADTARGET, y, NUM);
        saws_shrb_release(rb);

        shmem_barrier_all();
        //print_queue(rb);


        saws_shrb_t copy;
        shmem_getmem(&copy, rb, sizeof(saws_shrb_t), TAILTARGET);
        int to_steal = saws_shrb_shared_size(rb);
        int stolen = 0;
        int asteals = copy.steal_val >> 24;
        int steals =  (copy.steal_val >> 19) & 0x1F;
        cnt = 0;
        if(rb->procid == 0) {
            //printf("asteals: %d isteals %d\n", asteals, steals);
            printf("\n    attempting  %d  steals...\n", asteals);
        }
            //test stealing all elements, and then a failed steal
        for(i = 0; i < steals; i++) {
            int z;
            z = saws_shrb_pop_tail(rb, TAILTARGET, x);
            for (j = 0; j < z; j++) {
                shmem_fence();
                if (rb->procid == 0)
                    printf("      stole elems: %d %d\n", x[j].id, x[j].check);
            }
            if (rb->procid == 0) printf("\n");

            stolen += z;
            asteals--;
        }

        if (stolen != to_steal) {
            errors++;
            printf(" *** test failed --  pop_tail()***\n");
        }


        shmem_barrier_all();
        if (DEBUG) {
            for(int i = 0; i < NUM; i++) {
                printf("%d: q[%d] = %d :: %d\n", _c->rank, i, x[i].id, x[i].check);			
            }
        }
        if (rb->procid == 0) {
            printf("\nIteration %d, %d errors --\n", rep, errors);
            if (DEBUG) 
                saws_shrb_print(rb);
            printf("\n");
        }
        saws_shrb_reset(rb);
        for (i = NUM, cnt = 0; saws_shrb_pop_head(rb, HEADTARGET, x) > 0; i--,cnt++) {
        }
    }

    shmem_barrier_all();

    saws_shrb_destroy(rb);
    gtc_fini();

    return 0;
}

