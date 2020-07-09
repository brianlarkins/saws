#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <shmem.h>
#include <tc.h>
#include <saws_shrb.h>

#define GTC_TEST_DEBUG   0
#define NUM    16
#define TAILTARGET  (rb->procid + 1) % rb->nproc /* round robin */
#define HEADTARGET  rb->procid               /* local only */
#define root (rb->procid == 0)
#define DEBUG 0
typedef struct {
    int    id;
    char   junk[100];
    int    check;
} elem_t;


/*************** global variables **********************/
int tests[20] = {2, 3, 4, 5, 7, 8, 9, 10, 12, 14, 16, 18, 20, 24, 30, 64, 100, 128, 1024,2048};
elem_t y[128];
int errors = 0;


void print_queue(saws_shrb_t *rb) {  
    elem_t *ela = (elem_t *) rb->q;   

    saws_shrb_print(rb);  
    for (int i = 0; i < saws_shrb_size(rb); i++)    
        printf("%d: q[%d] = %d :: %d\n", _c->rank, i, ela[i].id, ela[i].check);
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

uint64_t log_2(long val) {

    int newval = log10(val) / log10(2) + 1;
    if (newval < 0) return 0;
    return newval;

}


void test_release() {
  tc_t tc;

  memset(&tc, 0, sizeof(tc_t));

    eprintf("\nUNIT TEST: saws_shrb_release()\n");
    saws_shrb_t *rb; 
    elem_t *x;
    for(int rep = 0; rep < NUM; rep++) {
        eprintf("testing with queue of %d\n\n", tests[rep]); 
        rb = saws_shrb_create(sizeof(elem_t), tests[rep], &tc);
        x = saws_shrb_malloc(tests[rep], sizeof(elem_t));
        saws_shrb_reset(rb);
        saws_shrb_push_n_head(rb, HEADTARGET, y, tests[rep]);

        int nloc = rb->nlocal;
        saws_shrb_release(rb);

        shmem_barrier_all();
        assert(saws_shrb_shared_size(rb) == nloc / 2 + nloc % 2);

        uint64_t size = saws_shrb_shared_size(rb);
        shmem_fence();
        shmem_barrier_all();
        assert(((rb->steal_val >> 19) & 0x00000000007FFFF) == size);

        // steal all the tasks
        while(saws_shrb_pop_tail(rb, TAILTARGET, x))    ;
        shmem_barrier_all();
        saws_shrb_reclaim_space(rb);
        saws_shrb_release(rb);

        assert(saws_shrb_shared_size(rb) > 0);

    }
    //free(x);
    //saws_shrb_destroy(rb);
}


void test_reacquire() {
  tc_t tc;

  memset(&tc, 0, sizeof(tc_t));

    eprintf("\nUNIT TEST: saws_shrb_reacquire()\n\n");
    
    saws_shrb_t *rb; 

    for(int rep = 0; rep < NUM; rep++) {

        eprintf("   testing with queue of %d\n\n", tests[rep]); 
        
        rb = saws_shrb_create(sizeof(elem_t), tests[rep], &tc);
        elem_t *x;
        x = saws_shrb_malloc(tests[rep], sizeof(elem_t));
        saws_shrb_reset(rb);
        saws_shrb_push_n_head(rb, HEADTARGET, y, tests[rep]);

        saws_shrb_release(rb);
        shmem_barrier_all();
        assert(saws_shrb_shared_size(rb) == tests[rep] / 2 + tests[rep] % 2);

        // reacquire all the tests
        while (!saws_shrb_isempty(rb)) {
            int n = rb->nlocal; 
            for (int i = 0; i < n; i++)
                saws_shrb_pop_head(rb, HEADTARGET, x);
          shmem_barrier_all();
            saws_shrb_print(rb);
            assert(rb->nlocal == 0);

            saws_shrb_reacquire(rb);
            assert(rb->split == saws_shrb_shared_size(rb));
        
        }
    }
    saws_shrb_destroy(rb);
}


void test_steals() {
  tc_t tc;

  memset(&tc, 0, sizeof(tc_t));

    eprintf("\nUNIT TEST: saws_shrb_ pop_tail()\n\n");

    saws_shrb_t *rb; 
    elem_t *x;

    for(int rep = 0; rep < NUM; rep++) {

        eprintf("testing with queue of %d\n", tests[rep]); 
        rb = saws_shrb_create(sizeof(elem_t), tests[rep], &tc);
        x = saws_shrb_malloc(tests[rep], sizeof(elem_t));
        shmem_barrier_all();

        int steals;
        
        // test pop_tail from empty queue
        assert(saws_shrb_pop_tail(rb, TAILTARGET, x) == 0);

        for (int i = 0; i < tests[rep]; i++)
            y[i].id = y[i].check = tests[rep] - i;
        
        saws_shrb_push_n_head(rb, HEADTARGET, y, tests[rep]);

        shmem_barrier_all();
        
        // no tasks have been released yet.
        int i =  saws_shrb_pop_tail(rb, TAILTARGET, x);
        shmem_barrier_all();
        assert(i == 0);

        //test stealing all the tasks
        
        saws_shrb_release(rb);
        shmem_barrier_all();

        uint32_t count = 0;
        steals = shmem_atomic_fetch(&rb->steal_val, TAILTARGET);
        int itasks = (steals >> 19) & 0x1F;
        shmem_barrier_all();
        
        while (saws_shrb_pop_tail(rb, TAILTARGET, x)) {
            count++;
        }
        if (DEBUG) printf("(%d) completed %d steals on qsize of %d\n", rb->procid, count, itasks);
        shmem_barrier_all();
        
        // check correct number of steals occurred
        assert(count == log_2(itasks));

        saws_shrb_reclaim_space(rb);
        assert(saws_shrb_shared_isempty(rb));

        shmem_barrier_all(); 
        
    }
}

void test_reclaim() {

    eprintf("\nUNIT TEST saws_shrb_reclaim()\n\n");
    /*
       saws_shrb_reset(rb);

       saws_shrb_push_n_head(rb, HEADTARGET, y,  tests[rep]);
       shmem_barrier_all();

       saws_shrb_release(rb);
       shmem_barrier_all();

       if (1){printf("before pop\n"); saws_shrb_print(rb);}

       i = saws_shrb_pop_tail(rb, TAILTARGET, x);
       shmem_barrier_all();
       saws_shrb_print(rb);
       assert(rb->completed == 1);

       if (DBG) {printf(" %d tasks was stolen\n", i); saws_shrb_print(rb);}

       saws_shrb_reclaim_space(rb);

       int tl = rb->tail;
    //if (root) {printf(" %d tasks was stolen, after reclaim\n", i); saws_shrb_print(rb);}
    saws_shrb_reclaim_space(rb);
    shmem_barrier_all();
    assert(rb->tail == tl); //no space should have been freed
    if (DBG) {printf("after reclaim: \n"); saws_shrb_print(rb);}
    shmem_barrier_all();
    saws_shrb_destroy(rb);
    shmem_barrier_all();*/
}



/*
 * main function, runs all the tests.
 * as soon as something goes wrong the program gives up and dies.
 */
int main(int argc, char **argv, char **envp) {

    setbuf(stdout, NULL);

    _c = gtc_init();
    eprintf("\nSAWS ring buffer test: Started with %d threads\n\n", _c->size);
    shmem_barrier_all();

    test_release();

    test_reacquire();

    //test_steals();

    //test_reclaim();

   // eprintf("\n all tests passed %d\n", errors);

    shmem_barrier_all();
    gtc_fini();

    return 0;
}

