#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <shmem.h>
#include <tc.h>
#include <sdc_shr_ring.h>

#define GTC_TEST_DEBUG   0
// #define QSIZE   500
// #define NUM     203
// #define NUMREPS 100
#define QSIZE   50
#define NUM     10
#define NUMREPS 1
#define TAILTARGET  (rb->procid + 1) % rb->nproc /* round robin */
#define HEADTARGET  rb->procid               /* local only */

typedef struct {
  int    id;
  char   junk[100];
  int    check;
} elem_t;

void print_queue(sdc_shrb_t *rb) {  
  elem_t *ela = (elem_t *) rb->q;   
 	
  sdc_shrb_print(rb);  
  for (int i = 0; i < QSIZE; i++)    
    printf("%d: q[%d] = %d :: %d\n", _c->rank, i, ela[i].id, ela[i].check);
}

void print_buf(elem_t *a) {
  for (int i = 0; i < NUM; i++)
    printf("%d: q[%d] = %d :: %d\n", _c->rank, i, a[i].id, a[i].check);			
}

int main(int argc, char **argv, char **envp) {
  int errors = 0;
  int i, j, rep;
  int cnt, total;		
  sdc_shrb_t *rb;		// Ring buffer
  tc_t *tc = calloc(1,sizeof(tc_t));

  elem_t *e;
  elem_t x;
  elem_t y[NUM];

  setbuf(stdout, NULL);

  _c = gtc_init();			// Initialize environment

  shmem_barrier_all();

  rb = sdc_shrb_create(sizeof(elem_t), QSIZE, tc);

  if (rb->procid == 0) 
    printf("\nSHMEM Split deferred-copy shared ring buffer test: Started with %d threads\n", rb->nproc);

  for (rep = 0; rep < NUMREPS; rep++) {
    if (rb->procid == 0) {
      printf("\nIteration %d, %d errors --\n", rep, errors);
      // if (DEBUG) 
        // sdc_shrb_print(rb);
      printf("\n");
    }
      
    if (rb->procid == 0) printf(" TEST: push_head() -> pop_head()\n");
      
      for (i = 1; i <= NUM; i++) {
        y[0].id = y[0].check = i;
        sdc_shrb_push_head(rb, HEADTARGET, &y[0], sizeof(elem_t));
      }

      shmem_barrier_all();

      for (i=NUM,cnt=0; sdc_shrb_pop_head(rb, HEADTARGET, &x) > 0; i--,cnt++) {
        if (x.id != i && x.check != i) {
          printf("  -- %d: Error, got <%d, %d> expected <%d, %d>\n", rb->procid, x.id, x.check, i, i);
          ++errors;
        }
      }

      if (cnt < NUM) {
        printf("  -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
        ++errors;
      }

      shmem_barrier_all();

      // if (DEBUG) sdc_shrb_print(rb);
    if (rb->procid == 0) printf(" TEST: push_n_head() -> pop_head()\n");

        for (i = 0; i < NUM; i++)
            y[i].id = y[i].check = i;

        sdc_shrb_push_n_head(rb, HEADTARGET, y, NUM);

        shmem_barrier_all();

        for (i=NUM-1,cnt=0; sdc_shrb_pop_head(rb, HEADTARGET, &x) > 0; i--,cnt++) {
            if (x.id != i && x.check != i) {
                printf("  -- %d: Error, got <%d, %d> expected <%d, %d>\n", rb->procid, x.id, x.check, i, i);
                ++errors;
            }
        }

        if (cnt < NUM) {
          printf("  -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
          ++errors;
        }

    shmem_barrier_all();
    
    // if (DEBUG) sdc_shrb_print(rb);
    if (rb->procid == 0) printf(" TEST: push_n_head() -> pop_tail()\n");

        for (i = 0; i < NUM; i++)
            y[i].id = y[i].check = NUM-i;

        sdc_shrb_push_n_head(rb, HEADTARGET, y, NUM);
        sdc_shrb_release_all(rb);

        shmem_barrier_all();

        for (i=NUM,cnt=0; sdc_shrb_pop_tail(rb, TAILTARGET, &x); i--,cnt++) {
            if (x.id != i && x.check != i) {
                printf("  -- %d: Error, got <%d, %d> expected <%d, %d>\n", rb->procid, x.id, x.check, i, i);
                ++errors;
            }
            //else
            //    printf("(%d) got <%d, %d>, expected <%d, %d>\n", rb->procid, x.id, x.check, i, i);
            sdc_shrb_release(rb);
        }

        if (cnt < NUM) {
          printf("  -- %d: Error, got %d elements, expected %d\n", rb->procid, cnt, NUM);
          ++errors;
        }

    shmem_barrier_all();

    // if (DEBUG) sdc_shrb_print(rb);
    if (rb->procid == 0) printf(" TEST: push_head() -> pop_n_tail()\n");

        e = malloc(sizeof(elem_t) * NUM);
        
        for (i = 1; i <= NUM; i++) {
            y[0].id = y[0].check = i;
            sdc_shrb_push_head(rb, HEADTARGET, &y[0], sizeof(elem_t));
        }

        sdc_shrb_release_all(rb);
        shmem_barrier_all();

        total = 0;
        while ((cnt = sdc_shrb_pop_n_tail(rb, TAILTARGET, NUM, e, STEAL_HALF))) {
          // printf("  + pop_n_tail() got %d, total %d\n", cnt, total+cnt);
          for (i=0, j=total; i < cnt; i++, j++) {
            if (e[i].id != j+1 && e[i].check != j+1) {
              printf("  -- %d: Error, got <%d, %d> expected <%d, %d>\n", rb->procid, e[i].id, e[i].check, i, i);
              ++errors;
            }
            // else
              // printf("(%d) got <%d, %d>, expected <%d, %d>, j: %d\n", rb->procid, e[i].id, e[i].check, i, i, j);
          }
          total += cnt;
          sdc_shrb_release(rb);
          shmem_barrier_all();
        }

        free(e);

        if (total < NUM) {
          printf("  -- %d: Error, got %d elements, expected %d\n", rb->procid, total, NUM);
          ++errors;
        }

        shmem_barrier_all();
  }

  shmem_barrier_all();

  sdc_shrb_destroy(rb);
  // if (lock)
  //  shmem_free(lock);

  // shmem_finalize();         // Finalize OpenSHMEM
  gtc_fini();
  free(tc);

  return 0;
}

