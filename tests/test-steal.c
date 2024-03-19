#include "shmem.h"
#include "wctimer.h"
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include "tc.h"
#define HALFKB 512
#define KB HALFKB*(2) 
#define MB KB*(1024)
#define REPS 10000

int dbg_printf_real(const char *format, ...) {
  va_list ap;
  int ret, len;
  char buf[1024], obuf[1024];

  va_start(ap, format);
  ret = vsprintf(buf, format, ap);
  va_end(ap);
  len = sprintf(obuf, "%4d: %s", shmem_my_pe(), buf);
  write(STDOUT_FILENO, obuf, len);
  return ret;
}

int main(void) {
    shmem_init();
    wc_timer_t timer;
    wc_tsc_calibrate();
    int my_pe = shmem_my_pe();

    for (int i = 1; i <= 100000000; i *= 10) {
        char *q = shmem_malloc(i);
        char *our_q = malloc(i);
        
        WC_INIT_TIMER(timer);
        //WC_START_TIMER(timer);
        
        if (my_pe == 0) {
            for (int j = 0; j < REPS; j++) {
                WC_START_TIMER(timer);
                shmem_getmem(our_q, q, i, 1); // assuming two processes, one thief and one victim
                WC_STOP_TIMER(timer);
            }
        }
        shmem_barrier_all();
        //WC_STOP_TIMER(timer);
        
        if (my_pe == 0) {
            dbg_printf_real("copied %9d bytes in %15.8f usec\n", i, (WC_READ_TIMER_USEC(timer)/REPS));
        }

        shmem_free(q);
        free(our_q);
    }
    shmem_finalize();
}
