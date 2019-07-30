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
  char buf[1024], obuf[1024];

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
  char buf[1024], obuf[1024];

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
 * gtc_get_mmad() - get min/max/avg for doubles
 *
 * @param counter counter to reduce
 * @param min     reduced min value
 * @param max     reduced max value
 * @param avg     reduced average value
 */
void gtc_get_mmad(double *counter, double *tot, double *min, double *max, double *avg) {
  gtc_reduce(counter, max, GtcReduceOpMax, DoubleType, 1);
  gtc_reduce(counter, min, GtcReduceOpMin, DoubleType, 1);
  gtc_reduce(counter, tot, GtcReduceOpSum, DoubleType, 1);
  *avg = *tot/((double)_c->size);
}



/**
 * gtc_get_mmau() - get min/max/avg for unsigned longs
 *
 * @param counter counter to reduce
 * @param min     reduced min value
 * @param max     reduced max value
 * @param avg     reduced average value
 */
void gtc_get_mmau(tc_counter_t *counter, tc_counter_t *tot, tc_counter_t *min, tc_counter_t *max, double *avg) {
  gtc_reduce(counter, max, GtcReduceOpMax, UnsignedLongType, 1);
  gtc_reduce(counter, min, GtcReduceOpMin, UnsignedLongType, 1);
  gtc_reduce(counter, tot, GtcReduceOpSum, UnsignedLongType, 1);
  *avg = *tot/((double)_c->size);
}



/**
 * gtc_get_mmal() - get min/max/avg for signed longs
 *
 * @param counter counter to reduce
 * @param min     reduced min value
 * @param max     reduced max value
 * @param avg     reduced average value
 */
void gtc_get_mmal(long *counter, long *tot, long *min, long *max, double *avg) {
  gtc_reduce(counter, max, GtcReduceOpMax, LongType, 1);
  gtc_reduce(counter, min, GtcReduceOpMin, LongType, 1);
  gtc_reduce(counter, tot, GtcReduceOpSum, LongType, 1);
  *avg = *tot/((double)_c->size);
}



/**
 * gtc_print_mmad() - print min/max/avg for doubles
 *
 * @param buf     buffer to place output string
 * @param unit    string for printing units
 * @param stat    our local value of the stat
 * @param total   print total count or not?
 */
char *gtc_print_mmad(char *buf, char *unit, double stat, int total) {
  double tot, min, max, avg;
  gtc_get_mmad(&stat, &tot, &min, &max, &avg);
  if (total)
    sprintf(buf, "%6.2f%s (%6.2f%s/%6.2f%s/%6.2f%s)", tot, unit, avg, unit, min, unit, max, unit);
  else
    sprintf(buf, "%6.2f%s/%6.2f%s/%6.2f%s", avg, unit, min, unit, max, unit);
  return buf;
}



/**
 * gtc_print_mmau() - print min/max/avg for unsigned longs
 *
 * @param buf     buffer to place output string
 * @param unit    string for printing units
 * @param stat    our local value of the stat
 * @param total   print total count or not?
 */
char *gtc_print_mmau(char *buf, char *unit, tc_counter_t stat, int total) {
  tc_counter_t tot, min, max;
  double  avg;
  gtc_get_mmau(&stat, &tot, &min, &max, &avg);
  if (total)
    sprintf(buf, "%6lu%s (%6.2f%s/%3lu%s/%3lu%s)", tot, unit, avg, unit, min, unit, max, unit);
  else
    sprintf(buf, "%6.2f%s/%3lu%s/%3lu%s", avg, unit, min, unit, max, unit);
  return buf;
}



/**
 * gtc_print_mmal() - print min/max/avg for signed longs
 *
 * @param buf     buffer to place output string
 * @param unit    string for printing units
 * @param stat    our local value of the stat
 * @param total   print total count or not?
 */
char *gtc_print_mmal(char *buf, char *unit, long stat, int total) {
  long tot, min, max;
  double  avg;
  gtc_get_mmal(&stat, &tot, &min, &max, &avg);
  if (total)
    sprintf(buf, "%3lu%s (%6.2f%s/%3lu%s/%3lu%s)", tot, unit, avg, unit, min, unit, max, unit);
  else
    sprintf(buf, "%6.2f%s/%3lu%s/%3lu%s", avg, unit, min, unit, max, unit);
  return buf;
}
