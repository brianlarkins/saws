/**********************************************************/
/*                                                        */
/*    tensor.h                                            */
/*                                                        */
/*    author: d. brian larkins                            */
/*    id: $Id: tensor.h 611 2007-03-06 15:28:37Z brian $  */
/*                                                        */
/**********************************************************/

#ifndef _TENSOR_H
#define _TENSOR_H

#include <assert.h>

#define TENSOR_MAXDIM    3
#define TENSOR_DEFAULT_K 9
#define TENSOR_ZERO      1
#define TENSOR_NOZERO    0

struct tensorhdr_s {
  long    size;                       // # of elements in tensor
  long    ndim;                       // # of dimensions (-1=invalid,0=scalar,>0=tensor)
  long    dim[TENSOR_MAXDIM];         // size of each dimension
  long    stride[TENSOR_MAXDIM];      // increment between elts in each dim
  double *pointer;                    // used for local tensor ops (copy,transpose,slice etc.)
};
typedef struct tensorhdr_s tensorhdr_t;

struct tensor_s {
  tensorhdr_t h;
  double      array[0];
};
typedef struct tensor_s tensor_t;

/* static speciailizations for common case */

struct tensor1dk_s {
  tensorhdr_t h;
  double      array[TENSOR_DEFAULT_K];
};
typedef struct tensor1dk_s tensor1dk_t;

struct tensor2dk_s {
  tensorhdr_t h;
  double      array[TENSOR_DEFAULT_K*TENSOR_DEFAULT_K];
};
typedef struct tensor2dk_s tensor2dk_t;

struct tensor3dk_s {
  tensorhdr_t h;
  double      array[TENSOR_DEFAULT_K*TENSOR_DEFAULT_K*TENSOR_DEFAULT_K];
};
typedef struct tensor3dk_s tensor3dk_t;

struct tensor1d2k_s {
  tensorhdr_t h;
  double      array[TENSOR_DEFAULT_K];
};
typedef struct tensor1d2k_s tensor1d2k_t;

struct tensor2d2k_s {
  tensorhdr_t h;
  double      array[2*TENSOR_DEFAULT_K*TENSOR_DEFAULT_K];
};
typedef struct tensor2d2k_s tensor2d2k_t;

struct tensor3d2k_s {
  tensorhdr_t h;
  double      array[2*TENSOR_DEFAULT_K*TENSOR_DEFAULT_K*TENSOR_DEFAULT_K];
};
typedef struct tensor3d2k_s tensor3d2k_t;

struct slice_s {
  long start;
  long end;
  long step;
};
typedef struct slice_s slice_t;


/* function prototypes */
tensor_t *tensor_create1d(long d0, int zero);
tensor_t *tensor_create2d(long d0, long d1, int zero);
tensor_t *tensor_create3d(long d0, long d1, long d2, int zero);
tensor_t *tensor_init1d(tensor_t *t,long d0, int zero);
tensor_t *tensor_init2d(tensor_t *t, long d0, long d1, int zero);
tensor_t *tensor_init3d(tensor_t *t, long d0, long d1, long d2,int zero);
static inline double tensor_get1d(tensor_t *t, long i);
static inline double tensor_get2d(tensor_t *t, long i, long j);
static inline double tensor_get3d(tensor_t *t, long i, long j, long k);
static inline double tensor_set1d(tensor_t *t, long i, double val);
static inline double tensor_set2d(tensor_t *t, long i, long j, double val);
static inline double tensor_set3d(tensor_t *t, long i, long j, long k, double val);

slice_t *slice_create(long s, long e);
void     slice_free(slice_t *slice);
void     slice_reverse(slice_t *slice);

static inline double tensor_get1d(tensor_t *t, long i) {
  return t->array[i*t->h.stride[0]];
}

static inline double tensor_get2d(tensor_t *t, long i, long j) {
  return t->array[i*t->h.stride[0]+j*t->h.stride[1]];
}

static inline double tensor_get3d(tensor_t *t, long i, long j, long k) {
  return t->array[i*t->h.stride[0]+j*t->h.stride[1]+k*t->h.stride[2]];
}

static inline double tensor_set1d(tensor_t *t, long i, double val) {
  return t->array[i*t->h.stride[0]] = val;
}

static inline double tensor_set2d(tensor_t *t, long i, long j, double val) {
  return t->array[i*t->h.stride[0]+j*t->h.stride[1]] = val;
}

static inline double tensor_set3d(tensor_t *t, long i, long j, long k, double val) {
  return t->array[i*t->h.stride[0]+j*t->h.stride[1]+k*t->h.stride[2]] = val;
}

tensor_t *tensor_hcopy(tensor_t *src, tensorhdr_t *t, int freeme);
tensor_t *tensor_copy(tensor_t *src);
tensor_t *tensor_scale(tensor_t *t, double v);
void tensor_fillindex(tensor_t *t);
void tensor_print(tensor_t *t, int vals);

#define talloc malloc
#define tfree  free

#endif // _TENSOR_H

