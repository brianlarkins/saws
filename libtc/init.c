/********************************************************/
/*                                                      */
/*  init.c - scioto openshmem initialization            */
/*     (c) 2020 see COPYRIGHT in top-level              */
/*                                                      */
/********************************************************/

#define _GNU_SOURCE 1
#include <alloca.h>
#include <assert.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <ucontext.h>

#include <tc.h>

int __gtc_marker[5] = { 0, 0, 0, 0, 0};

static void gtc_exit_handler(void);

gtc_context_t *_c;
gtc_context_t *_sanity;
int gtc_is_initialized = 0;



/**
 * gtc_init - initializes sciotwo system
 */
gtc_context_t *gtc_init(void) {

  // turn off output buffering for everyone's sanity
  setbuf(stdout, NULL);

  // allocate context structure
  _c = (gtc_context_t *)calloc(1, sizeof(gtc_context_t));

  // set gdb backtraces if possible
  setenv("SHMEM_BACKTRACE", "gdb", 1);

  // initialize openshmem
  shmem_init();

  _c->rank = shmem_my_pe();
  _c->size = shmem_n_pes();

  _c->total_tcs = -1;
  for (int i=0; i< GTC_MAX_TC; i++) {
    _c->tcs[i] = NULL;
  }
  _c->dbglvl = GTC_DEFAULT_DEBUGLEVEL;
  _c->quiet  = 1;

  _c->auto_teardown = (gtc_is_initialized == -1) ? 1 : 0;
  gtc_is_initialized = 1; // mark ourselves as initialized in either case

  atexit(gtc_exit_handler);

  // register backtrace handler
  struct sigaction sa;
  sa.sa_sigaction = (void *)gtc_bthandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_SIGINFO;

  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGALRM, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  _c->tsc_cpu_hz = gtc_tsc_calibrate();

  _sanity = _c;

  return _c;
}



/**
 * gtc_fini - initializes sciotwo system
 */
void gtc_fini(void) {
  shmem_finalize();
  free(_c);
  _c = NULL;
}



static void gtc_exit_handler(void) {
   printf("\n rank %d exited normally\n", _c->rank);
}



/*
 * backtrace handler
 */
void gtc_bthandler(int sig, siginfo_t *si, void *vctx) {
  void *a[100];
  char sprog[256], str[256];
  size_t size;
  ucontext_t *ctx = (ucontext_t *)vctx;
  char **msgs = NULL, *pstr = NULL;
  FILE *fp = NULL;

  UNUSED(si);

  size = backtrace(a, 100);
  printf("rank: %d pid : %d signal: %d marker: %d %d %d %d %d\n", _c->rank, getpid(), sig,
      __gtc_marker[0], __gtc_marker[1], __gtc_marker[2], __gtc_marker[3], __gtc_marker[4]);
  printf("func: %s  file %s:%d\n", _sanity->curfun, _sanity->curfile, _sanity->curline);
  fflush(stdout);

  //backtrace_symbols_fd(a,size, STDERR_FILENO);
  msgs = backtrace_symbols(a, size);
  a[1] = (void *)ctx->uc_mcontext.gregs[REG_RIP];
  for (size_t i=1; i<size; i++) {
    size_t p = 0;
    while (msgs[i][p] != '(' && msgs[i][p] != ' ' && msgs[i][p] != 0)
      p++;
    sprintf(sprog, "/usr/bin/addr2line %p -e %.*s 2>&1", a[i], (int)p, msgs[i]);
    //fp = popen(sprog, "r");
    if (fp) {
      printf("%d: %d %d %p %p\n", _c->rank, (int)p, (int)size, str, fp); fflush(stdout);
      pstr = fgets(str, 256, fp);
      printf("%d: %d %d%5s\n", _c->rank, (int)p, (int)size, str); fflush(stdout);
      if (pstr && (str[0] != '?'))
        gtc_dprintf(" (backtrace) #%d %s\n\t%s", i, msgs[i], str);
      pclose(fp);
    } else {
        gtc_dprintf(" (backtrace) #%d %s\n", i, msgs[i]);
    }
  }
  exit(1);
}



/**
 * Set the behavior of the load balancer.
 */
void gtc_ldbal_cfg_set(gtc_t gtc, gtc_ldbal_cfg_t *cfg) {
  tc_t *tc = gtc_lookup(gtc);

  assert(cfg->target_selection == TARGET_RANDOM || cfg->target_selection == TARGET_ROUND_ROBIN);
  assert(cfg->steal_method == STEAL_HALF || cfg->steal_method == STEAL_ALL || cfg->steal_method == STEAL_CHUNK);
  assert(cfg->max_steal_retries >= 0);
  assert(cfg->max_steal_attempts_local >= 0);
  assert(cfg->max_steal_attempts_remote >= 0);
  assert(cfg->chunk_size >= 1);
  assert(cfg->local_search_factor >= 0 && cfg->local_search_factor <= 100);

  tc->ldbal_cfg = *cfg;
}



/** Get the behavior of the load balancer.
  */
void gtc_ldbal_cfg_get(gtc_t gtc, gtc_ldbal_cfg_t *cfg) {
  tc_t *tc = gtc_lookup(gtc);

  *cfg = tc->ldbal_cfg;
}


/**
 * Set up a ldbal_cfg struct with the default values.
 */
void gtc_ldbal_cfg_init(gtc_ldbal_cfg_t *cfg) {
  cfg->stealing_enabled    = 1;
  cfg->target_selection    = TARGET_RANDOM;
  cfg->steal_method        = STEAL_HALF;
  cfg->steals_can_abort    = 1;
  cfg->max_steal_retries   = 5;
  cfg->max_steal_attempts_local = 1000;
  cfg->max_steal_attempts_remote= 10;
  cfg->chunk_size          = 1;
  cfg->local_search_factor = 75;
}
