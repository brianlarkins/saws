/********************************************************/
/*                                                      */
/*  init.c - scioto openshmem initialization            */
/*     (c) 2021 see COPYRIGHT in top-level              */
/*                                                      */
/********************************************************/

#define _GNU_SOURCE 1
#include <assert.h>
#include <dlfcn.h>
#include <link.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <ucontext.h>

#include <tc.h>
#include "buildinfo.h"

int __gtc_marker[5] = { 0, 0, 0, 0, 0};

static void gtc_exit_handler(void);
static void gtc_print_build_info(void);
static int  gtc_find_libsma(struct dl_phdr_info *info, size_t size, void *data);

gtc_context_t *_c;
int gtc_is_initialized = 0;



/**
 * gtc_init - initializes sciotwo system
 */
gtc_context_t *gtc_init(void) {

  // turn off output buffering for everyone's sanity
  setbuf(stdout, NULL);

  // allocate context structure
  _c = (gtc_context_t *)calloc(1, sizeof(gtc_context_t));
  _c->allocsize += sizeof(gtc_context_t);

  // set gdb backtraces if possible
  setenv("SHMEM_BACKTRACE", "gdb", 1);

  // initialize openshmem
  shmem_init();

  _c->rank = shmem_my_pe();
  _c->size = shmem_n_pes();

  // per-task-path timers are off unless asked for (they cost ~7% on UTS)
  char *ft = getenv("GTC_FULL_TIMING");
  _c->full_timing = (ft && atoi(ft) != 0);

  // node membership for locality-aware stealing (collective)
  gtc_locality_init();

  if (_c->rank == 0) {
    gtc_print_build_info();
    gtc_locality_print();
    printf("SCIOTO timing: per-task-path timers %s\n",
           _c->full_timing ? "on (GTC_FULL_TIMING)" : "off (set GTC_FULL_TIMING=1 to enable)");
  }

#ifdef SCIOTO_TRACING
  if (_c->rank == 0)
    printf("      gtc_calloc: gtc_init of %ld\n", sizeof(gtc_context_t));
#endif

  _c->total_tcs = 0;
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

  return _c;
}



/**
 * gtc_fini - initializes sciotwo system
 */
void gtc_fini(void) {
  shmem_barrier_all();
  gtc_locality_fini();
  shmem_finalize();
  free(_c);
  _c = NULL;
}



/**
 * gtc_print_build_info - record which libtc and OpenSHMEM a run used.
 *
 * Printed once by rank 0 so that every output file identifies its build:
 * libtc revision, the OpenSHMEM install the library was compiled against,
 * the OpenSHMEM and UCX libraries actually loaded at run time, and whether
 * on-node shared memory (SHMEM_TEAM_SHARED / shmem_ptr) is available.
 */
static void gtc_print_build_info(void) {
  char name[SHMEM_MAX_NAME_LEN] = "unknown";
  int  major = 0, minor = 0;
  const char *ucx = "n/a";
  const char *(*ucx_version)(void);
  const char *sma_path = "static or unknown";
  int nshared, ptr_ok = 0;
  char *tls = getenv("UCX_TLS");
  char *amode = getenv("UCX_ATOMIC_MODE");

  shmem_info_get_name(name);
  shmem_info_get_version(&major, &minor);

  // UCX is a dependency of SOS, not of libtc, so look it up at run time
  ucx_version = (const char *(*)(void))dlsym(RTLD_DEFAULT, "ucp_get_version_string");
  if (ucx_version)
    ucx = ucx_version();

  dl_iterate_phdr(gtc_find_libsma, &sma_path);

  nshared = shmem_team_n_pes(SHMEM_TEAM_SHARED);
  if (nshared > 1) {
    int peer = shmem_team_translate_pe(SHMEM_TEAM_SHARED, 1, SHMEM_TEAM_WORLD);
    ptr_ok = (peer >= 0) && (shmem_ptr(&_c->rank, peer) != NULL);
  }

  printf("SCIOTO build : libtc %s, compiled against %s\n", GTC_BUILD_REV, GTC_BUILD_SHMEM);
  printf("SCIOTO shmem : %s (spec %d.%d), libsma %s, UCX %s\n", name, major, minor, sma_path, ucx);
  printf("SCIOTO onnode: SHMEM_TEAM_SHARED %d PEs, shmem_ptr %s, UCX_TLS=%s UCX_ATOMIC_MODE=%s\n",
         nshared, ptr_ok ? "yes" : "no", tls ? tls : "(default)", amode ? amode : "(default)");
}



/* dl_iterate_phdr callback: report the path of the loaded libsma */
static int gtc_find_libsma(struct dl_phdr_info *info, size_t size, void *data) {
  UNUSED(size);
  if (info->dlpi_name && strstr(info->dlpi_name, "libsma")) {
    *(const char **)data = info->dlpi_name;
    return 1;
  }
  return 0;
}



static void gtc_exit_handler(void) {
  if (_c)
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
  printf("func: %s  file %s:%d\n", _c->curfun, _c->curfile, _c->curline);
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

  assert(cfg->target_selection >= 0 && cfg->target_selection < GTC_NUM_TARGET_METHODS);
  assert(cfg->steal_method == STEAL_HALF || cfg->steal_method == STEAL_ALL || cfg->steal_method == STEAL_CHUNK);
  assert(cfg->max_steal_retries >= 0);
  assert(cfg->max_steal_attempts_local >= 0);
  assert(cfg->max_steal_attempts_remote >= 0);
  assert(cfg->chunk_size >= 1);
  assert(cfg->local_search_factor >= 0 && cfg->local_search_factor <= 100);
  assert(cfg->local_sweep_far >= 1);

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
  cfg->local_sweep_far     = 16;
}



/**
 * Apply environment overrides to a load balancer configuration, so experiments
 * can change the target policy without changing the applications:
 *   GTC_TARGET     random | rr | local-prob | local-sweep
 *   GTC_LOCAL_PCT  local-prob: percent of attempts aimed at node-mates (0..100)
 *   GTC_SWEEP_FAR  local-sweep: off-node attempts between node sweeps (>= 1)
 * Unknown or out-of-range values abort, so a typo cannot silently run the default.
 */
void gtc_ldbal_cfg_env(gtc_ldbal_cfg_t *cfg) {
  char *t   = getenv("GTC_TARGET");
  char *pct = getenv("GTC_LOCAL_PCT");
  char *far = getenv("GTC_SWEEP_FAR");

  if (t) {
    if      (!strcmp(t, "random"))      cfg->target_selection = TARGET_RANDOM;
    else if (!strcmp(t, "rr"))          cfg->target_selection = TARGET_ROUND_ROBIN;
    else if (!strcmp(t, "local-prob"))  cfg->target_selection = TARGET_LOCAL_PROB;
    else if (!strcmp(t, "local-sweep")) cfg->target_selection = TARGET_LOCAL_SWEEP;
    else {
      fprintf(stderr, "GTC_TARGET=%s: expected random, rr, local-prob, or local-sweep\n", t);
      abort();
    }
  }
  if (pct) {
    cfg->local_search_factor = atoi(pct);
    if (cfg->local_search_factor < 0 || cfg->local_search_factor > 100) {
      fprintf(stderr, "GTC_LOCAL_PCT=%s: expected 0..100\n", pct);
      abort();
    }
  }
  if (far) {
    cfg->local_sweep_far = atoi(far);
    if (cfg->local_sweep_far < 1) {
      fprintf(stderr, "GTC_SWEEP_FAR=%s: expected >= 1\n", far);
      abort();
    }
  }
}
