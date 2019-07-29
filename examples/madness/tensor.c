/**********************************************************/
/*                                                        */
/*    tensor.c                                            */
/*                                                        */
/*    author: d. brian larkins                            */
/*    id: $Id: tensor.c 618 2007-03-06 23:53:38Z dinan $  */
/*                                                        */
/**********************************************************/

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tensor.h"

// add constructors to take base address
// add routine to determine sizeof a later allocation?

static inline void set_dims_and_size(tensor_t *t, long nd, const long d[]);
static inline int  iscontiguous(tensorhdr_t *h);

static void set_dims_and_size(tensor_t *t, long nd, const long d[]) {
  long i;
  t->h.ndim = nd;
  t->h.size = 1;
  if (nd < 0)
    t->h.size = 0;
  for (i=t->h.ndim-1; i>=0; i--) {
    t->h.dim[i] = d[i];
    t->h.stride[i] = t->h.size;
    t->h.size *= d[i];
  }

  for (i=nd; i<TENSOR_MAXDIM; i++) {
    t->h.dim[i] = 1;
    t->h.stride[i] = 0;
  }
}


static inline int iscontiguous(tensorhdr_t *h) {
  long i, size=1;
  if (h->size == 0)
    return 1;
  for (i=h->ndim-1; i>=0;i--) {
    if (h->stride[i] != size)
      return 0;
    size *= h->dim[i];
  }
  return 1;
}


tensor_t *tensor_create1d(long d0, int zero) {
  tensor_t *t = NULL;
  size_t datasize = 0;

  datasize = d0 * sizeof(double);
  t = talloc(sizeof(tensor_t)+datasize);
  tensor_init1d(t,d0,zero);
  return t;
}



tensor_t *tensor_create2d(long d0, long d1, int zero) {
  tensor_t *t = NULL;
  size_t datasize = 0;

  datasize = (d0 * d1) * sizeof(double);
  t = talloc(sizeof(tensor_t)+datasize);
  tensor_init2d(t,d0,d1,zero);

  return t;
}



tensor_t *tensor_create3d(long d0, long d1, long d2, int zero) {
  tensor_t *t = NULL;
  size_t datasize = 0;

  datasize = (d0 * d1 * d2) * sizeof(double);
  t = talloc(sizeof(tensor_t)+datasize);
  tensor_init3d(t,d0,d1,d2,zero);

  return t;
}



tensor_t *tensor_init1d(tensor_t *t, long d0, int zero) {
  long i;

  t->h.dim[0] = d0;
  set_dims_and_size(t,1,t->h.dim);

  // zero
  if (zero) {
    for (i=0; i<t->h.size; i++)
      t->array[i] = 0.0;
  }
  return t;
}



tensor_t *tensor_init2d(tensor_t *t, long d0, long d1, int zero) {
  long i;

  t->h.dim[0] = d0;
  t->h.dim[1] = d1;
  set_dims_and_size(t,2,t->h.dim);

  // zero
  if (zero) {
    for (i=0; i<t->h.size; i++)
      t->array[i] = 0.0;
  }  
  return t;
}



tensor_t *tensor_init3d(tensor_t *t, long d0, long d1, long d2,int zero) {
  long i;

  t->h.dim[0] = d0;
  t->h.dim[1] = d1;
  t->h.dim[2] = d2;
  set_dims_and_size(t,3,t->h.dim);

  // zero
  if (zero) {
    for (i=0; i<t->h.size; i++)
      t->array[i] = 0.0;
  }
  return t;
}


void tensor_free(tensor_t *t) {
  tfree(t);
}

slice_t *slice_create(long s, long e) {
  slice_t *sl = talloc(sizeof(slice_t));
  sl->start = s;
  sl->end   = e;
  sl->step  = 1;
  return sl;
}



void slice_free(slice_t *slice) {
  tfree(slice);
}



void slice_reverse(slice_t *slice) {
}





/*
 * copy a slice of a tensor and return it
 */
tensorhdr_t *tensor_slice(tensor_t *t, slice_t s[]) {
  long nd=0, size=1;
  long i, start, end, step, len;
  tensorhdr_t *this = talloc(sizeof(tensorhdr_t));

  for (i=0; i<t->h.ndim; i++) {
    start = s[i].start;
    end   = s[i].end;
    step  = s[i].step;

    if (start < 0) 
      start += t->h.dim[i]; // no elements
    if (end < 0) 
      end += t->h.dim[i];   // all elements

    len = end-start+1;

    if (step) 
      len /= step;	// Rounds len towards zero

    // if input length is not exact multiple of step, round end towards start
    // for the same behaviour of for (i=start; i<=end; i+=step);
    end = start + (len-1)*step;

    this->pointer += start * t->h.stride[i];

    if (step) {
      size *= len;
      this->dim[nd] = len;
      this->stride[nd] = step * t->h.stride[i];
      nd++;
    }
  }
  for (i=nd; i<TENSOR_MAXDIM; i++) { // So can iterate over missing dimensions
    this->dim[i] = 1;
    this->stride[i] = 0;
  }
  this->ndim = nd;
  this->size = size;

  return this;
}


/*
 * inplace scaling of tensor t by v
 *
 */
tensor_t *tensor_scale(tensor_t *t, double v) {
  int i;
  for (i=0;i<t->h.size;i++)
    t->array[i] *= v;
  return t;
}



tensor_t *tensor_transpose(tensor_t *src, tensor_t *dst) {
  tensorhdr_t *th = NULL;
  long tmp;

  // swapdim(0,1);
    
  th = talloc(sizeof(tensorhdr_t));
  memcpy(th, &src->h, sizeof(tensorhdr_t));
  
  tmp = th->dim[0];
  th->dim[0] = th->dim[1];
  th->dim[1] = tmp;

  tmp = th->stride[0];
  th->stride[0] = th->stride[1];
  th->stride[1] = tmp;
  return tensor_hcopy(src, th, 1);
}


/*
 * do a deep-copy of a tensor, based on a tensor header
 */
tensor_t *tensor_hcopy(tensor_t *src, tensorhdr_t *t, int freeme) {
  tensor_t *result = NULL;
  long i;
  double *p0, *p1;
  
  switch (t->ndim) {
  case 1:
    result = tensor_create1d(t->dim[0], TENSOR_NOZERO);
    break;
  case 2:
    result = tensor_create2d(t->dim[0], t->dim[1], TENSOR_NOZERO);
    break;
  case 3:
    result = tensor_create3d(t->dim[0], t->dim[1], t->dim[2], TENSOR_NOZERO);
    break;
  }
  
  if (!result) {
    printf("tensor_hcopy: illegal dimension: %ld\n", t->ndim);
    return NULL;
  }

  // copy tensor header to result tensor
  memcpy(&result->h, t, sizeof(tensorhdr_t));

  // need to take into consideration starting pointer
  if (iscontiguous(t) && iscontiguous(&(src->h))) {
    p0 = result->h.pointer;
    p1 = src->h.pointer;
    for (i=0; i<t->size; i++,p0++,p1++)
      *p0 = *p1;
  } else {
    printf("non-contiguous tensor copying not support yet.\n");
    tensor_free(result);
    result = NULL;
  }
  if (freeme)
    tfree(t);

  return result;
}



/*
 * do a full, deep-copy of a tensor
 */
tensor_t *tensor_copy(tensor_t *src) {
  tensor_t *result = NULL;
  long i;
  double *p0, *p1;
  
  switch (src->h.ndim) {
  case 1:
    result = tensor_create1d(src->h.dim[0],TENSOR_NOZERO);
    break;
  case 2:
    result = tensor_create2d(src->h.dim[0], src->h.dim[1],TENSOR_NOZERO);
    break;
  case 3:
    result = tensor_create3d(src->h.dim[0], src->h.dim[1], src->h.dim[2],TENSOR_NOZERO);
    break;
  }
  
  if (!result) {
    printf("tensor_copy: illegal dimension: %ld\n", src->h.ndim);
    return NULL;
  }

  // copy tensor header to result tensor
  memcpy(&result->h, src, sizeof(tensorhdr_t));

  p0 = result->array;
  p1 = src->array;
  assert(p0);
  assert(p1);
  for (i=0; i<src->h.size; i++,p0++,p1++)
    *p0 = *p1;

  return result;
}


void tensor_fillindex(tensor_t *t) {
  int i;

  for (i=0;i<t->h.size;i++)
    t->array[i] = (double)i;
}


void tensor_print(tensor_t *t, int vals) {
  int i,j,k;
#if 0
  //long datasize = 0;
  if (t->h.ndim == 3) {
    datasize = (t->h.dim[0] * t->h.dim[1] * t->h.dim[2]) * sizeof(double);
    printf("size: %ld ndim: %ld datasize: %ld\n", t->h.size, t->h.ndim, datasize+sizeof(tensor_t));
  } else {
    printf("size: %ld ndim: %ld\n", t->h.size, t->h.ndim);
  }
#endif

  printf("size: %ld ndim: %ld\n", t->h.size, t->h.ndim);

  for (i=0;i<t->h.ndim;i++) {
    printf("  dim   [%d]: %ld\n",i,t->h.dim[i]);
    printf("  stride[%d]: %ld\n",i,t->h.stride[i]);
  }
  
  if (!vals)
    return;
  
  switch (t->h.ndim) {
  case 1:
    for(i=0;i<t->h.dim[0];i++) {
	printf("%f ", tensor_get1d(t,i));
    }
    printf("\n");
    break;
  case 2:
    for(i=0;i<t->h.dim[0];i++) {
      for (j=0;j<t->h.dim[1];j++) {
	printf("%f ", tensor_get2d(t,i,j));
      }
      printf("\n");
    }
    break;
  case 3:
    for(i=0;i<t->h.dim[0];i++) {
      for (j=0;j<t->h.dim[1];j++) {
	printf("[%d,%d,*] ",i,j);
	for (k=0;k<t->h.dim[2]; k++)
	  printf("%10.4e ", tensor_get3d(t,i,j,k));
	printf("\n");
      }
    }
    printf("\n");
    break;
  }
}

