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
#define NUM     21
#define NUMREPS 1
#define TAILTARGET  (rb->procid + 1) % rb->nproc /* round robin */
#define HEADTARGET  rb->procid               /* local only */

long *lock;     // Global lock for synchronization

typedef struct {
  int    id;
  char   junk[100];
  int    check;
} elem_t;

void print_queue(sdc_shrb_t *rb) {  
  elem_t *ela = (elem_t *) rb->q;   
 	
  int rank = shmem_my_pe();

  sdc_shrb_print(rb);  
  for (int i = 0; i < QSIZE; i++)    
    printf("%d: q[%d] = %d :: %d\n", rank, i, ela[i].id, ela[i].check);	// Line fix (_c->rank)
}

void print_buf(elem_t *a) {
  int rank = shmem_my_pe();
    
  for (int i = 0; i < NUM; i++)
    printf("%d: q[%d] = %d :: %d\n", rank, i, a[i].id, a[i].check);			// Line fix
}

int main(int argc, char **argv, char **envp) {
  int errors = 0;
  int i, j, rep;
  int cnt, total;		
  sdc_shrb_t *rb;		// Ring buffer

  elem_t *e;
  elem_t x;
  elem_t y[NUM];

  setbuf(stdout, NULL);

  shmem_init();			// Initialize OpenSHMEM

  int my_pe = shmem_my_pe();
  int npes = shmem_n_pes();

  lock = (long *)shmem_malloc(sizeof(long) * npes);
  if (lock == NULL)
    printf("Allocation Error\n");
  
  shmem_barrier_all();

  rb = sdc_shrb_create(sizeof(elem_t), QSIZE);

  if (rb->procid == 0) 
    printf("\nSHMEM Split deferred-copy shared ring buffer test: Started with %d threads\n", rb->nproc);

  for (rep = 0; rep < NUMREPS; rep++) {
    if (rb->procid == 0) {
      printf("\nIteration %d, %d errors --\n", rep, errors);
      // if (DEBUG) 
        // sdc_shrb_print(rb);
      printf("\n");
    }
      
    if (rb->procid == 0) 
      printf(" TEST: push_head() -> pop_head()\n");
      
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

  }

  shmem_barrier_all();

  sdc_shrb_destroy(rb);
  if (lock)
    shmem_free(lock);

  shmem_finalize();         // Finalize OpenSHMEM

  return 0;
}
