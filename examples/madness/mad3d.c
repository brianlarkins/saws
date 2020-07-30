#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <shmem.h>

#include <tc.h>

#include "tensor.h"
#include "mad3d.h"
#include "tree.h"
#include "mad_analytics.h"

//#define MAD_QUEUE_SIZE    4096
#define MAD_QUEUE_SIZE    35000

#define NUM_AFCNS 4
static const afcn_t afcn_ptrs[NUM_AFCNS] = { lattice, lj_lattice, hydrogen, metal };
static const char   afcn_names[NUM_AFCNS][128] =
{ "Lattice",
  "Lennard-Jones Lattice",
  "Potential for Hydrogen Atom",
  "Valence Electron State for Metals" };

static int analytic_fcn = 0;

#define MAX_REFINE_LEVEL      30
#define DEFAULT_K             TENSOR_DEFAULT_K
#define DEFAULT_THRESHOLD     1e-3
//#define DEFAULT_THRESHOLD     .01
#define DEFAULT_INITIAL_LEVEL 5

#define TASK_AFFINITY_HIGH 0
#define TASK_AFFINITY_LOW  1
#define TASK_PRIORITY      0
#define NO_INPLACE 1

/********************************/
/* global shared variables      */
/********************************/

static task_class_t  refine_tclass;
static gtc_t         madtc;
static int           me, nproc;
static int           verbose   = 0;
static double        threshold = DEFAULT_THRESHOLD;
static int           init_level= DEFAULT_INITIAL_LEVEL;
static int           order_k   = DEFAULT_K; // Must change in tensor.h and recompile
gtc_qtype_t          qtype = GtcQueueSDC;

func_t *f;


/********************************/
/* protos                       */
/********************************/

func_t   *init_function(int k, double thresh, int initial_level, double (* analytic_fcn)(double x, double y, double z));
void      project(func_t *f, tree_t *node);
void      refine(func_t *f, tree_t *node);
tensor_t *gather_scaling_coeffs(func_t *f, tree_t *node);

void      create_refine_task(func_t *f, tree_t *node);
void      refine_task_wrapper(gtc_t gtc, task_t *closure);


func_t *init_function(int k, double thresh, int initial_level, double (* analytic_fcn)(double x, double y, double z)) {
  func_t *fun;
  int     i;

  fun = malloc(sizeof(func_t));
  fun->k         = k;
  fun->npt       = k;
  fun->thresh    = thresh;
  fun->f         = analytic_fcn;
  fun->initial_level = initial_level;
  fun->max_level = MAX_REFINE_LEVEL;

  for (i=0; i<NDIM; i++) {
    fun->s0[i] = fun->s[0];
    fun->vk[i] = fun->k;
    fun->vq[i] = fun->npt;
    fun->v2k[i] = 2*fun->k;
  }
  fun->work1 = tensor_create3d(fun->vk[0],fun->vk[1],fun->vk[2],TENSOR_ZERO);
  fun->work2 = tensor_create3d(fun->v2k[0], fun->vk[1], fun->vk[2],TENSOR_ZERO);
  fun->workq = tensor_create3d(fun->vq[0], fun->vq[1], fun->vq[2],TENSOR_ZERO);

  if (me == 0) printf(" + Initializing twoscale, quadrature, dc_periodic\n");
  init_twoscale(fun);
  init_quadrature(fun);
  make_dc_periodic(fun);

  fun->compressed = 0;
  fun->ftree = create_tree();       // Create tree and allocate root node 
  shmem_barrier_all();

  if (fun->f) {
    // Cheat a little: set global func_t *f for tasks to use
    f = fun;

    if (me == 0) {
      // Create a task for the root of the tree
      printf(" + Spawning root task\n");
      //create_refine_task(fun, get_root(fun->ftree));
      refine(fun, get_root(fun->ftree));
    }
    shmem_barrier_all();

    if (me == 0) printf(" + Processing task pool\n");
    gtc_process(madtc);
  }

  return fun;
}


void refine(func_t *f, tree_t *node) {
  long i, j, k;
  long x, y, z;
  long level = get_level(f->ftree, node);
  tensor_t   *ss, *sf;
  double      dnorm;

  get_xyzindex(f->ftree, node, &x, &y, &z);

  // if (verbose) 
  // printf("   [%3d] Refine: level = %ld, box = (%ld, %ld, %ld)\n", me, level, x, y, z);

  // Max level reached, can't refine any further
  if (level > f->max_level) {
    if (verbose) printf("   [%3d] WARNING: Refine hit max_level. level = %ld, box = (%ld, %ld, %ld)\n", me, level, x, y, z);
    return;
  }

  // Check if we are below the minimum level of refinement in the tree.  Don't bother
  // creating coefficients until we reach this level.
  if (level < f->initial_level) {
    // Set level and index info on my children
    set_children(f->ftree, node);

    // Create tasks for my children
    for (i = 0; i < 8; i++) {
      if (level < (f->initial_level - 1))
        refine(f, get_child(f->ftree, node, i));
      else
        create_refine_task(f, get_child(f->ftree, node, i));
    }
    return;
  }

  //
  // Project to level n+1
  //

  // Set level and index info on my children at level+1
  set_children(f->ftree, node);

  // Project to level+1
  project(f, node);

  //
  // Find the Frobenius norm of the coefficients at level n+1 and check
  // if we've reached the desired accuracy.
  //

  ss = gather_scaling_coeffs(f, node);

  sf = filter(f, ss);

  // fill(ss, 0.0);
  for (i = 0; i < f->k; i++)
    for (j = 0; j < f->k; j++)
      for (k = 0; k < f->k; k++)
        tensor_set3d(sf,i,j,k,0.0);

  dnorm = normf(sf);

  tfree(ss); ss = NULL;
  tfree(sf); sf = NULL;

  // Compare the Frobenius norm with user-specified threshold to see if
  // we need to refine further
  if (dnorm > f->thresh) {
    if (verbose) printf("   [%3d] Refining further: level = %ld, box = (%ld, %ld, %ld)\n", me, level, x, y, z);

    // Create tasks for my children
    for (i = 0; i < 8; i++)
      create_refine_task(f, get_child(f->ftree, node, i));

    // Wipe parent's scaling coefficients, they've been pushed down to children
    set_scaling(f->ftree, node, NULL);
  } else {
    if (verbose) printf("   [%3d] Box: level = %ld, box = (%ld, %ld, %ld)\n", me, level, x, y, z);
  }
}


/*
 * Projects function f->f down one level to children of node
 */
void project(func_t *f, tree_t *node) {
  long      level   = get_level(f->ftree, node);
  tensor_t *scoeffs = NULL, *tscoeffs = NULL;
  double    h       = 1.0/pow(2.0,level+1);
  double    scale   = sqrt(h);
  tree_t   *cnode;
  long      lx,ly,lz,ix,iy,iz;
  double    xlo,ylo,zlo;
  //long      twoton = pow(2.0,level);

  scale = scale*scale*scale;

  get_xyzindex(f->ftree, node, &lx, &ly, &lz);

  if (verbose) printf("    [%3d] Projecting scaling coeffs: level = %ld, box = (%ld, %ld, %ld)\n", me, level, lx, ly, lz);

  lx *= 2; ly *= 2; lz *= 2;

  scoeffs = tensor_create3d(f->npt, f->npt, f->npt, TENSOR_NOZERO);

  // for each child of node
  for (ix = 0; ix < 2; ix++) {
    xlo = (lx+ix)*h;
    for (iy = 0; iy < 2; iy++) {
      ylo = (ly+iy)*h;
      for (iz = 0; iz < 2; iz++) {
        zlo = (lz+iz)*h;
        cnode = get_child(f->ftree, node, ix*4+iy*2+iz);

        fcube(f, f->npt, xlo, ylo, zlo, h, f->f, scoeffs); // f->quad_x read through f
        tensor_scale(scoeffs, scale);
        tscoeffs = transform3d(scoeffs, f->quad_phiw);
        set_scaling(f->ftree, cnode, tscoeffs);
        tfree(tscoeffs);
      }
    }
  }
  tfree(scoeffs);
}


void create_refine_task(func_t *f, tree_t *node) {
#ifdef TEST_EVAL
  refine(f, node);

#else
#ifdef NO_INPLACE
  task_t     *task    = gtc_task_create(refine_tclass);
#else
  task_t     *task    = gtc_task_inplace_create_and_add(madtc, refine_tclass);
#endif
  mad_task_t *madtask = (mad_task_t *)gtc_task_body(task);
  // mad_task_t *madtask = malloc(sizeof(mad_task_t));
  // printf("(%d) sizeof madtask: %ld\n", _c->rank, sizeof(madtask));


  // Copy relevant bits into the task descriptor's body
  madtask->level = get_level(f->ftree, node);
  get_xyzindex(f->ftree, node, &madtask->x, &madtask->y, &madtask->z);

#ifdef NO_INPLACE
  gtc_add(madtc, task, me);
  gtc_task_destroy(task);
#endif
  gtc_task_inplace_create_and_add_finish(madtc, task);
#endif /* TEST_EVAL */
}



void refine_task_wrapper(gtc_t gtc, task_t *closure) {
  mad_task_t *t    = (mad_task_t *)gtc_task_body(closure);
  tree_t     *node = node_alloc(t->level, t->x, t->y, t->z);

  refine(f, node);

  node_free(node);
}


/* 
 * gather scaling coeffs for node at level n from coeffs at n+1
 *
 * ss[ 0:9,  0:9,  0:9] = +0,+0,+0
 * ss[ 0:9,  0:9, 9:18] = +0,+0,+1
 * ss[ 0:9, 9:18,  0:9] = +0,+1,+0
 * ss[ 0:9, 9:18, 9:18] = +0,+1,+1
 * ss[9:18,  0:9,  0:9] = +1,+0,+0
 * ss[9:18,  0:9, 9:18] = +1,+0,+1
 * ss[9:18, 9:18,  0:9] = +1,+1,+0
 * ss[9:18, 9:18, 9:18] = +1,+1,+1
 *
 */
tensor_t *gather_scaling_coeffs(func_t *f, tree_t *node) {
  //long level = get_level(f->ftree, node);
  tensor_t *ss = NULL;
  tensor_t *childsc = NULL;
  tree_t   *cnode = NULL;
  long ix,iy,iz, ixlo,iylo,izlo;
  long i,j,k;
  double t;

  ss = tensor_create3d(2*f->k,2*f->k,2*f->k, TENSOR_ZERO);


  // for each child
  for (ix = 0; ix < 2; ix++) {
    ixlo = ix*f->k;
    for (iy = 0; iy < 2; iy++) {
      iylo = iy*f->k;
      for (iz = 0; iz < 2; iz++) {
        izlo    = iz*f->k;
        cnode   = get_child(f->ftree, node, ix*4+iy*2+iz);
        childsc = get_scaling(f->ftree, cnode);
        assert(childsc);

        // copy child scaling coeffs into ss
        for (i = 0; i < f->k; i++) {
          for (j = 0;j < f->k; j++) {
            for (k = 0;k < f->k; k++) {
              t = tensor_get3d(childsc, i, j, k);
              tensor_set3d(ss, ixlo+i, iylo+j, izlo+k, t);
            }
          }
        }
        tfree(childsc); childsc = NULL;
      }
    }
  }
  return ss;
}


/* 
 * evaluate f(x,y,z) madness3/mra/cfcube.c
 */
double eval(func_t *f, tree_t *node, double x, double y, double z) {
  long level;
  double px[100],py[100],pz[100], sum = 0.0;
  tree_t *curnode = NULL;
  tensor_t *s = NULL;
  long ix, iy, iz;
  long p,q,r;
  long index;
  double xx,yy,zz;
  double *ptr;
  double twon = 1, twoinv = 1;
  double aa,bb,cc;

  curnode = node;

  while (!has_scaling(f->ftree, curnode)) {
    // recur down tree from root until we find scaling coeffs
    // find the child containing the spatial box with x,y,z
    get_xyzindex(f->ftree, curnode, &ix, &iy, &iz);
    level = get_level(f->ftree, curnode);

    if (level > f->max_level)
      return 0.0;

    index = 0;

    //printf("eval %ld:%ld,%ld,%ld for %f,%f,%f \n",level,ix,iy,iz,x,y,z);

    // #nodes on next level down
    twon = pow(2.0, level+1);
    twoinv = 1.0/twon;

    ix *= 2; iy *= 2; iz *= 2;

    aa = ((double) ix)*twoinv;
    bb = ((double) iy)*twoinv;
    cc = ((double) iz)*twoinv;

    if (z > (cc+twoinv)) {
      index += 1;
      iz +=1;
    }

    if (y > (bb+twoinv)) {
      index += 2;
      iy +=1;
    }

    if (x > (aa+twoinv)) {
      index += 4;
      ix +=1;
    }

    curnode = get_child(f->ftree, curnode, index);
  }

  level = get_level(f->ftree, curnode);

  // found scaling coeffs, compute function value.
  s = get_scaling(f->ftree, curnode);
  ptr = s->array; // tensor abstraction violation... 

  // hoodoo magic
  xx = x * twon; yy = y * twon; zz = z * twon;
  xx = xx - ix; yy = yy - iy; zz = zz - iz;

  phi(xx,f->k,px);
  phi(yy,f->k,py);
  phi(zz,f->k,pz);

  for (p=0;p<f->k;p++) {
    for (q=0;q<f->k;q++) {
      for (r=0;r<f->k;r++) {
        sum = sum + *ptr++ * px[p]*py[q]*pz[r];
      }
    }
  }
  tfree(s);
  return sum*pow(2.0,1.5*level);
}


void process_args(int argc, char **argv) {
  int   arg;
  char *endptr;
  int pernode = 0;

  while ((arg = getopt(argc, argv, "t:i:f:lvhBHp")) != -1) {
    switch (arg) {
      case 't':
        threshold = strtod(optarg, &endptr);
        if (endptr == optarg) {
          printf("Error, invalid threshold: %s\n", optarg);
          exit(1);
        }
        break;

      case 'i':
        init_level = strtol(optarg, &endptr, 10);
        if (endptr == optarg) {
          printf("Error, invalid initial level: %s\n", optarg);
          exit(1);
        }
        break;

      case 'f':
        analytic_fcn = strtol(optarg, &endptr, 10);
        if (endptr == optarg || analytic_fcn >= NUM_AFCNS) {
          printf("Error, invalid analytic function: %s\n", optarg);
          exit(1);
        }
        break;

      case 'l':
        if (me == 0) {
          int i;
          printf("Available analytic functions:\n");
          for (i = 0; i < NUM_AFCNS; i++) {
            printf("\t%d: %s\n", i, afcn_names[i]);
          }
        }
        exit(0);
        break;

      case 'v':
        verbose = 1;
        break;

      case 'h':
        if (me == 0) {
          printf("SCIOTO Parallel 3-D Madness -- Tree Creation Kernel\n");
          printf("  Usage: %s [args]\n\n", basename(argv[0]));
          printf("Options:\n");
          printf("  -t double       Refinement threshold (e.g. 10e-3)\n");
          printf("  -i int          Initial level of refinement (e.g. 0-%d)\n", MAX_REFINE_LEVEL);
          printf("  -f int          Select analytic function\n");
          printf("  -l              List analytic functions\n");
          printf("  -v              Verbose output\n");
          printf("  -h              Help\n");
        }
        exit(0);
        break;

      case 'B':
        qtype = GtcQueueSDC;
        break;
      case 'H':
        qtype = GtcQueueSAWS;
        break;
      case 'p':
        pernode = 1;
        break;

      default:
        if (me == 0) printf("Try '-h' for help.\n");
        exit(1);
    }
  }

  if (!pernode)
    setenv("SCIOTO_DISABLE_PERNODE_STATS", "1", 1);
}


int main(int argc, char **argv, char **envp) {
  afcn_t afcn;

  //setenv("SCIOTO_CORE_BINDING", "socket", 1);
  gtc_init();

  me    = _c->rank;
  nproc = _c->size;

  process_args(argc, argv);

  if (me == 0) printf("Madness 3d Tree Creation Kernel: Scioto task-parallel on %d cores, Analytic funcion: %s\n\n", 
      nproc, afcn_names[analytic_fcn]);

  madtc         = gtc_create(sizeof(mad_task_t), 10, MAD_QUEUE_SIZE, NULL, qtype);
  refine_tclass = gtc_task_class_register(sizeof(mad_task_t), refine_task_wrapper);

  //
  // parallel tree creation
  //

  if (me == 0) printf("Initializing function tree: thresh=%e k=%d initial_level=%d.\n",
      threshold, order_k, init_level);

  afcn = afcn_ptrs[analytic_fcn];
  f    = init_function(order_k, threshold, init_level, afcn);

  if (me == 0) printf("Initializing function tree complete.\n");

#ifdef TEST_EVAL
  // Note: This program will run sequentially when you enable TEST_EVAL
  if (me == 0) {
    int npt = 5;
    int i, j, k;
    for (i = 0; i < npt; i++)
      for (j = 0; j < npt; j++)
        for (k = 0; k < npt; k++) {
          double numerical = eval(f, f->ftree->root, i/(double)npt, j/(double)npt, k/(double)npt);
          double analytic  = afcn(i/(double)npt, j/(double)npt, k/(double)npt);
          printf("(%0.4f, %0.4f, %0.4f) -- Numerical=%8.5f Analytic=%8.5f Err=%8.5f\n", i/(double)npt,
              j/(double)npt, k/(double)npt, numerical, analytic, numerical-analytic);
        }
  }
#endif

  gtc_print_stats(madtc);
  shmem_barrier_all();
  gtc_destroy(madtc);
  gtc_fini();

  fflush(NULL);

  return 0;
}

