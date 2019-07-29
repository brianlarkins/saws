#ifndef _mad3d_h
#define _mad3d_h

#include "tree.h"

#define MAD_CHILD_LEFT  0
#define MAD_CHILD_RIGHT 1

#define MAX_NODE_LIMIT 20
#define MAX_TREE_DEPTH 10

#define DEFAULT_CHUNKSIZE 256

#define NDIM 3

// Analytic function pointer
typedef double (*afcn_t)(double x, double y, double z);

/* function structure */
struct func_s {
  int        k;              // first k legendre polynomials {k==npt}
  int        npt;            // number of quadrature points
  double     thresh;         // truncation threshold for wavelet coeffs
  afcn_t     f;              // analytic function
  int        max_level;      // don't refine below this level
  int        initial_level;  // Initial level of refinement
  int        compressed;     // tree compressed? or not

  slice_t    s[4];           // Slice(0,k-1), s[1]=Slice(k,2*k-1), etc.
  slice_t    s0[NDIM];       // s[0] in each dimension
  long       vk[NDIM];       // k,... tensor initialization
  long       v2k[NDIM];      // 2k,...  "
  long       vq[NDIM];       // npt,... "
  tensor_t  *work1;          // workspace of (k,...)
  tensor_t  *work2;          // workspace of (2k,...)
  tensor_t  *workq;          // workspace of (npt,...)
  // tensor<double> zero_coeff = convenience of diff?
  tensor_t  *hg;             // twoscale relation
  tensor_t  *hgT;            // transpose
  tensor_t  *hgsonly;	     // hg[0:k,:]
  tensor_t  *quad_w;         // quadrature weights
  tensor_t  *quad_x;         // quadrature points
  tensor_t  *quad_phi;       // quad_phi(i,j) = at x[i] value of phi[j]
  tensor_t  *quad_phiT;      // transpose of quad_phi
  tensor_t  *quad_phiw;      // quad_phiw(i,j) = at x[i] value of w[i]*phi[j]
  tensor_t  *rm;             // minus block for derivative operator
  tensor_t  *r0;             // self  block for derivative operator
  tensor_t  *rp;             // plus  block for derivative operator
  tensor_t  *rm_left;        // left block for f->rm

  tensor_t  *rm_right;       // right block for f->rm
  tensor_t  *rp_left;        // left block for f->rp
  tensor_t  *rp_right;       // right block for f->rp
  mad_tree_t *ftree;          // global function tree
}; 
typedef struct func_s func_t;


struct mad_task_s {
  long level;
  long x, y, z;
};
typedef struct mad_task_s mad_task_t;


// global private vars
extern double h0[9][9];
extern double g0[9][9];
extern double quad_points[9];
extern double quad_weights[9];
extern double phi_norms[100];

// init.c
void           init_quadrature(func_t *f);
void           make_dc_periodic(func_t *f);
void           init_twoscale(func_t *f);
tensor_t      *two_scale_hg(int k);

// math.c
void           pn(double x, int order, double *p);
void	       phi(double x, long k, double *p);
tensor_t      *filter(func_t *f, tensor_t *s);
tensor_t      *unfilter(func_t *f, tensor_t *ss, int sonly);
tensor_t      *transform(tensor_t *t, tensor_t *c);
tensor_t      *transform3d(tensor_t *t, tensor_t *c);
tensor_t      *transform3d_inplace(func_t *f, tensor_t *s, tensor_t *c, tensor_t *work);
tensor_t      *inner(tensor_t *left, tensor_t *right, long k0, long k1, tensor_t *inplace);
double         truncate_tol(func_t *f, double tol, long level);
void           filter_inplace(func_t *f, tensor_t *s);
double         normf(tensor_t *t);
void           mTxm(long dimi, long dimj, long dimk, double *c, double *a, double *b);
void           fcube(func_t *f, long n, double lx, double ly, double lz, double h, double (*fn)(double p ,double q, double r), tensor_t *fcube);
void           math_test(void);

#endif // _mad3d_h

