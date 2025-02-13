#include "shmem.h"
// #include <tc.h>
#include "wctimer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define SIZE 1024
#define UTS_SIZE 52
#define REPS 100000

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
  wc_timer_t intranode;
  wc_timer_t internode;
  wc_timer_t both;
  wc_tsc_calibrate();

  // double avg_time;

  int my_pe = shmem_my_pe();
  // int npes = shmem_n_pes();

  // allocate buffer to simulate having 1024 uts tasks
  char *uts = shmem_malloc(UTS_SIZE * 1024);

  // first try stealing from an on-node process

  // actually on second thought: I'd like to see the difference between
  // intranode and internode stealing cumulatively
  if (my_pe == 0) {
    dbg_printf_real("multi-steal test (intranode):\n");
    WC_INIT_TIMER(intranode);
    WC_START_TIMER(intranode);
    for (int i = 0; i < REPS; i++) {
      shmem_getmem(uts, uts, UTS_SIZE * 16, 1);
    }
    WC_STOP_TIMER(intranode);

    dbg_printf_real("Total time of steals (intranode): %g\n",
                    WC_READ_TIMER_USEC(intranode));

    dbg_printf_real("multi-steal test (internode):\n");
    WC_INIT_TIMER(internode);
    WC_START_TIMER(internode);
    for (int i = 0; i < REPS; i++) {
      shmem_getmem(uts, uts, UTS_SIZE * 16, 49);
    }
    WC_STOP_TIMER(internode);

    dbg_printf_real("Total time for steals (internode): %g\n",
                    WC_READ_TIMER_USEC(internode));

    dbg_printf_real("combined test:\n");
    WC_INIT_TIMER(both);
    WC_START_TIMER(both);
    for (int i = 0; i < REPS * 0.4; i++) {
      shmem_getmem(uts, uts, UTS_SIZE * 16, 1);
    }
    for (int i = 0; i < REPS * 0.6; i++) {
      shmem_getmem(uts, uts, UTS_SIZE * 16, 49);
    }
    WC_STOP_TIMER(both);

    dbg_printf_real("Total time for steals (both): %g\n",
                    WC_READ_TIMER_USEC(both));
  }
  /*if (my_pe == 0) {*/
  /*  dbg_printf_real("Intranode steal test:\n");*/
  /*  for (int i = 1; i <= 1024; i *= 2) {*/
  /*    WC_INIT_TIMER(intranode);*/
  /*    for (int j = 0; j < REPS; j++) {*/
  /*      WC_START_TIMER(intranode);*/
  /*      // dbg_printf_real("before getmem\n");*/
  /*      shmem_getmem(uts, uts, UTS_SIZE * i, 1);*/
  /*      // dbg_printf_real("after getmem\n");*/
  /*      WC_STOP_TIMER(intranode);*/
  /*    }*/
  /**/
  /*    avg_time = WC_READ_TIMER_USEC(intranode) / REPS;*/
  /**/
  /*    if (my_pe == 0) {*/
  /*      dbg_printf_real("Avg time taken (%d tasks): %g\n", i, avg_time);*/
  /*    }*/
  /*  }*/
  /**/
  /*  dbg_printf_real("Internode steal test:\n");*/
  /*  for (int i = 1; i <= 1024; i *= 2) {*/
  /*    WC_INIT_TIMER(internode);*/
  /*    for (int j = 0; j < REPS; j++) {*/
  /*      WC_START_TIMER(internode);*/
  /*      shmem_getmem(uts, uts, UTS_SIZE * i, 49);*/
  /*      WC_STOP_TIMER(internode);*/
  /*    }*/
  /**/
  /*    avg_time = WC_READ_TIMER_USEC(internode) / REPS;*/
  /**/
  /*    if (my_pe == 0) {*/
  /*      dbg_printf_real("Avg time taken (%d tasks): %g\n", i, avg_time);*/
  /*    }*/
  /*    // calculate average and print*/
  /*  }*/
  /*}*/

  shmem_barrier_all();
  shmem_finalize();
}
