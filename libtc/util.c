#define _GNU_SOURCE 1
#include <time.h>

#include <tc.h>

/**
 *  eprintf - error printing wrapper
 *    @return number of bytes written to stderr
 */
int eprintf(const char *format, ...) {
  va_list ap;
  int ret;

  if (_c->rank == 0) {
    va_start(ap, format);
    ret = vfprintf(stdout, format, ap);
    va_end(ap);
    fflush(stdout);
    return ret;
  }
  else
    return 0;
}



/**
 *  gtc_dbg_printf - optionally compiled debug printer
 *    @return number of bytes written to stderr
 */
int gtc_dbg_printf(const char *format, ...) {
  va_list ap;
  int len, ret;
  char buf[1024], obuf[1234];

  va_start(ap, format);
  ret = vsprintf(buf, format, ap);
  va_end(ap);
  len = sprintf(obuf, "%4d: %s", _c->rank, buf);
  len = write(STDOUT_FILENO, obuf, len);
  /*
  fprintf(stdout, "%d: %s", _c->rank, buf);
  fflush(stdout);
  */
  return ret;
}



/**
 *  gtc_lvl_dbg_printf - optionally compiled debug printer with priority level
 *    @param lvl priority level
 *    @param format format string
 *    @return number of bytes written to stderr
 */
int gtc_lvl_dbg_printf(int lvl, const char *format, ...) {
  va_list ap;
  int len, ret = 0;
  char buf[1024], obuf[1234];

  if (lvl &= _c->dbglvl) {
    va_start(ap, format);
    ret = vsprintf(buf, format, ap);
    va_end(ap);
    len = sprintf(obuf, "%4d: %s", _c->rank, buf);
    len = write(STDOUT_FILENO, obuf, len);
    /*
    fprintf(stdout, "%d: %s", _c->rank, buf);
    fflush(stdout);
    */
  }
  return ret;
}



/**
 * gtc_tsc_calibrate - calibrate the TSC timer
 * @return the calculated MHz of the current PE for tick-to-ns conversion
 */
double gtc_tsc_calibrate(void) {
  struct timespec start, end, sleep;
  uint64_t tscstart, tscend, tscdiff, clockdiff;
  double tscticks;

  // calibrate TSC timer
  sleep.tv_sec = 0;
  sleep.tv_nsec = 25 * 1000000L; // 25ms
  tscticks = 0.0;
  tscdiff = clockdiff = 0;

  // run calibration test 10 times (250 ms)
  // also do sanity checking against clock_gettime()
  for (int i=0; i<10; i++) {
    memset(&start, 0, sizeof(start));
    memset(&end, 0, sizeof(end));

    start = gtc_get_wtime();
    tscstart = gtc_get_tsctime();
    nanosleep(&sleep, NULL);
    tscend   = gtc_get_tsctime();
    end = gtc_get_wtime();
    end.tv_sec  -= start.tv_sec;
    end.tv_nsec -= start.tv_nsec;
    clockdiff += (1000000000L * (end.tv_sec)) + end.tv_nsec; // should be 25M ns
    tscdiff   += (tscend - tscstart);

    // 25ms * 40 == 1000ms == 1sec
    tscticks += ((tscend - tscstart) * 40) / (double) 1e6;
  }

  tscticks /= 10.0;

  if (_c->rank == 0) {
    // clockdiff and tscdiff should be ~250M ns or 250 ms
    gtc_lprintf(DBGINIT, "gtc_tsc_calibrate: calibrated MHz: %7.3f clock_gettime: %7.3f ms rtdsc: %7.3f ms\n",
        tscticks,
        (clockdiff/(double)1e6),
        ((tscdiff/tscticks)/(double)1e3));
  }
  return tscticks;
}



/**
 *  gtc_lvl_dbg_printf - optionally compiled debug printer with priority level
 *    @param lvl priority level
 *    @param format format string
 *    @return number of bytes written to stderr
 */
int gtc_lvl_dbg_eprintf(int lvl, const char *format, ...) {
  va_list ap;
  int ret = 0;

  if ((_c->rank == 0) && (lvl &= _c->dbglvl)) {
    va_start(ap, format);
    ret = vfprintf(stdout, format, ap);
    va_end(ap);
    fflush(stdout);
  }
  return ret;
}


/**
 * gtc_sanity_check
 */
void gtc_sanity_check(void) {

  if (_c->rank != _sanity->rank)          { gtc_dprintf("rank corrupted\n"); }
  if (_c->size != _sanity->size)          { gtc_dprintf("size corrupted\n"); }
  for (int i=0; i<_c->total_tcs; i++) {
    if (_c->tcs[i] != _sanity->tcs[i])    { gtc_dprintf("tc[%d] corrupted\n", i); }
    switch (_c->tcs[i]->qtype) {
      case GtcQueueSDC:
        //gtc_sdc_sanity(_c->tcs[i], _sanity->tcs[i]);
        break;
      case GtcQueueSAWS:
        //gtc_saws_sanity(_c->tcs[i], _sanity->tcs[i]);
        break;
    }
  }
}

#ifdef GTC_USE_OLD_SHMEM_COLLECTIVES
typedef enum { GtcReduceMin, GtcReduceMax, GtcReduceSum } gtc_reduce_op_t;
typedef enum { GtcTypeLong, GtcTypeDouble } gtc_reduce_type_t;
static  long   psync[SHMEM_REDUCE_SYNC_SIZE];
static  long   longwork[10+SHMEM_REDUCE_MIN_WRKDATA_SIZE];
static  double doublework[10+SHMEM_REDUCE_MIN_WRKDATA_SIZE];

static void gtc_reduce(void *dest, void *source, size_t nreduce, gtc_reduce_op_t op, gtc_reduce_type_t ty) {

  for (int i=0; i<SHMEM_REDUCE_SYNC_SIZE; i++)
    psync[i] = SHMEM_SYNC_VALUE;
  shmem_barrier_all();

  switch (ty) {
    case GtcTypeLong:
      {
        switch (op) {
          case GtcReduceMin:
            gtc_dprintf("lmin\n");
            shmem_long_min_to_all((long *)dest, (long *)source, nreduce, 0, 0, _c->size, longwork, psync);
            break;
          case GtcReduceMax:
            gtc_dprintf("lmax\n");
            shmem_long_max_to_all((long *)dest, (long *)source, nreduce, 0, 0, _c->size, longwork, psync);
            break;
          case GtcReduceSum:
            gtc_dprintf("lsum\n");
            shmem_long_sum_to_all((long *)dest, (long *)source, nreduce, 0, 0, _c->size, longwork, psync);
            break;
        }
      }
      break;
    case GtcTypeDouble:
      {
        for (size_t i=0; i<nreduce; i++) {
          gtc_dprintf("%d: %f\n", i, ((double *)source)[i]);
        }
        switch (op) {
          case GtcReduceMin:
            gtc_dprintf("dmin\n");
            shmem_double_min_to_all((double *)dest, (double *)source, nreduce, 0, 0, _c->size, doublework, psync);
            break;
          case GtcReduceMax:
            gtc_dprintf("dmax\n");
            shmem_double_max_to_all((double *)dest, (double *)source, nreduce, 0, 0, _c->size, doublework, psync);
            break;
          case GtcReduceSum:
            gtc_dprintf("dsum\n");
            shmem_double_sum_to_all((double *)dest, (double *)source, nreduce, 0, 0, _c->size, doublework, psync);
            break;
        }
      }
      break;
  }
  shmem_barrier_all();
  gtc_dprintf("done\n");
}

void gtc_min_reduce_uint64(uint64_t *dest, uint64_t *source, size_t nreduce) {
  gtc_reduce(dest, source, nreduce, GtcReduceMin, GtcTypeLong);
}

void gtc_max_reduce_uint64(uint64_t *dest, uint64_t *source, size_t nreduce) {
  gtc_reduce(dest, source, nreduce, GtcReduceMax, GtcTypeLong);
}

void gtc_sum_reduce_uint64(uint64_t *dest, uint64_t *source, size_t nreduce) {
  gtc_reduce(dest, source, nreduce, GtcReduceSum, GtcTypeLong);
}

void gtc_min_reduce_double(double *dest, double *source, size_t nreduce) {
  gtc_reduce(dest, source, nreduce, GtcReduceMin, GtcTypeDouble);
}

void gtc_max_reduce_double(double *dest, double *source, size_t nreduce) {
  gtc_reduce(dest, source, nreduce, GtcReduceMax, GtcTypeDouble);
}

void gtc_sum_reduce_double(double *dest, double *source, size_t nreduce) {
  gtc_reduce(dest, source, nreduce, GtcReduceSum, GtcTypeDouble);
}
#endif // GTC_USE_OLD_SHMEM_COLLECTIVES
