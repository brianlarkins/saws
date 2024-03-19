#include <stdio.h>
#include "shmem.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include "wctimer.h"
#define SIZE 100
#define REPS 100

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

    // calibrate and declare timers
    wc_tsc_calibrate();
    wc_timer_t intranode;
    wc_timer_t internode;

    int my_pe = shmem_my_pe();

    char *buf = shmem_malloc(SIZE);
    char *our_buf = malloc(SIZE);
    
    // initialize two timers, one for intranode stealing
    // and one for internode stealing
    WC_INIT_TIMER(intranode);
    WC_INIT_TIMER(internode);

    for (int i = 0; i < REPS; i++) {

        if (my_pe > 0 && my_pe < 48) {
            // start timer
            WC_START_TIMER(intranode);
            shmem_getmem(our_buf, buf, SIZE, 0);
            // stop timer
            WC_STOP_TIMER(intranode);
        }
        shmem_barrier_all();
    }

    for (int i = 0; i < REPS; i++) {
        if (my_pe > 48 && my_pe < 96) {
            // start timer
            WC_START_TIMER(internode);
            shmem_getmem(our_buf, buf, SIZE, 0);
            // stop timer
            WC_STOP_TIMER(internode);
        }
        shmem_barrier_all();
    }

    // reduce times we gathered from previous tests
    double *times = shmem_malloc(2 * sizeof(double));
    double *sum_times = shmem_malloc(2 * sizeof(double));
    times[0] = WC_READ_TIMER_USEC(intranode) / REPS; // intranode average
    times[1] = WC_READ_TIMER_USEC(internode) / REPS; // internode average
    shmem_sum_reduce(SHMEM_TEAM_WORLD, sum_times, times, 2); // reduce times to sum_times

    sum_times[0] /= 47;
    sum_times[1] /= 47;
    shmem_barrier_all();
    if (my_pe == 0) {
        dbg_printf_real("Average intranode steal time: %g usec\n", sum_times[0]);
        dbg_printf_real("Average internode steal time: %g usec\n", sum_times[1]);
    }

    shmem_finalize();
}
