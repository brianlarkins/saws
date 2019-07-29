/**********************************************************/
/*                                                        */
/*    math.c                                              */
/*                                                        */
/*    author: d. brian larkins                            */
/*    id: $Id: tensor.c 618 2007-03-06 23:53:38Z dinan $  */
/*                                                        */
/**********************************************************/


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "tensor.h"
#include "mad3d.h"

/*
 * compute phi twoscale coeffs
 *
 * source: madness3/mra/cfcube.c
 */
void phi(double x, long k, double *p) {
  static long phi_norms_init = 1;
  static double phi_norms[100];
  long n;

  if (phi_norms_init) {
    for (n=0; n<100; n++)
      phi_norms[n] = sqrt(2.0*n+1.0);
    phi_norms_init = 0;
  }
  pn(2.0*x-1,k-1,p);
  for (n=0;n<k;n++)
    p[n] = p[n]*phi_norms[n];
}



/*
 * compute phi norms for use in phi computation
 *
 * source: madness3/mra/cfcube.c
 */
void pn(double x, int order, double *p) {
  static double nn1[100];
  static long firstcall=1;
  long n;

  p[0] = 1.0;
  if (order == 0)
    return;

  if (firstcall) {
    for (n=0;n<100;n++)
      nn1[n] = n/((double) (n+1));
    firstcall = 0;
  }
  p[1] = x;
  for (n=1;n<order;n++)
    p[n+1] = (x*p[n] - p[n-1])*nn1[n] + x*p[n];
}


/* 
 * transform sums at level n to sums+differences at level n-1
 */
tensor_t *filter(func_t *f, tensor_t *s) {
  return transform3d(s,f->hgT);
}


/* 
 * transform sums+differences at level n to sums at level n+1
 */
tensor_t *unfilter(func_t *f, tensor_t *ss, int sonly) {
  if (sonly) 
    return transform(ss, f->hgsonly);
  else
    return transform3d(ss, f->hg);
}


/* 
 * transform sums+differences at level n to sums at level n+1
 */
void filter_inplace(func_t *f, tensor_t *s) {
  transform3d_inplace(f, s, f->hgT, f->work2);
}



/* 
 * transform sums+differences at level n to sums at level n+1
 */
void unfilter_inplace(func_t *f, tensor_t *s) {
  transform3d_inplace(f, s, f->hg, f->work2);
}


/*
 * transform all dimensions of the tensor t by the matrix c
 * madness/tensor/tensor.cc
 */
tensor_t *transform(tensor_t *t, tensor_t *c) {
  tensor_t *temp[4], *result = NULL;
  int i;

  temp[0] = t;
  temp[1] = NULL;
  temp[2] = NULL;
  temp[3] = NULL;

  for (i=0;i<t->h.ndim;i++) {
    // inner() allocates new tensors
    result = temp[i+1] = inner(temp[i],c,0,0,NULL);
  }
  
  for (i=1;i<4;i++) {
    if (!temp[i] && (result != temp[i])) 
      tfree(temp[i]);
  }

  return result;
}



/* 
 * 3d transformation, from madness/src/lib/tensor/transform3d.cpp:24
 */
tensor_t *transform3d(tensor_t *t, tensor_t *c) {
  long d0 = c->h.dim[0];
  long d0_squared = d0*d0;
  long d0_cubed = d0_squared*d0;
  tensor_t *result = tensor_create3d(d0,d0,d0,TENSOR_ZERO);
  double *tmp, *r_p, *t_p, *c_p, *tmp_p;
  long i;

  tmp   = talloc(d0_cubed*sizeof(double));
  r_p   = result->array;
  t_p   = t->array;
  c_p   = c->array;
  tmp_p = tmp;

  for (i=0;i<d0_cubed;i++)
    tmp[i] = 0.0;

  // both result and tmp are zero filled at this point
  // Transform along 1st dimension
  // result gets "result"
  mTxm(d0_squared, d0, d0, r_p, t_p, c_p);

  // Transform along 2nd dimension
  // tmp gets "result"
  mTxm(d0_squared, d0, d0, tmp_p, r_p, c_p);

  // Transform along 3rd dimension
  for (i=0;i<d0_cubed;i++)
    result->array[i] = 0.0;

  // result gets "result"
  mTxm(d0_squared, d0, d0, r_p, tmp_p, c_p);

  tfree(tmp);
  return result;
}




tensor_t *transform3d_inplace(func_t *f, tensor_t *s, tensor_t *c, tensor_t *work) {
  double *sptr = NULL; // XXX - need fixed
  double *wptr = NULL;
  double *cptr = NULL;
  long k2 = 2*f->k;
  long k2sq = k2*k2;
  long k2cu = k2sq*k2;
  int  i;

  for (i=0;i<k2cu;i++) wptr[i] = 0.0;
  mTxm(k2sq, k2, k2, wptr, sptr, cptr);
  for (i=0;i<k2cu;i++) sptr[i] = 0.0;
  mTxm(k2sq, k2, k2, sptr, wptr, cptr);
  for (i=0;i<k2cu;i++) wptr[i] = 0.0;
  mTxm(k2sq, k2, k2, wptr, sptr, cptr);
  for (i=0;i<k2cu;i++) sptr[i] = 0.0;
  
  return s;
}



double normf(tensor_t *t) {
  double result = 0.0, temp;
  long i,j,k;

  switch (t->h.ndim) {
  case 1:
    for (i=0;i<t->h.dim[0];i++) {
      temp = tensor_get1d(t,i);
      result += temp * temp;
    }
    break;

  case 2:
    for (i=0;i<t->h.dim[0];i++) {
      for (j=0;j<t->h.dim[1];j++) {
	temp = tensor_get2d(t,i,j);
	result += temp * temp;
      }
    }
    break;

  case 3:
    for (i=0;i<t->h.dim[0];i++) {
      for (j=0;j<t->h.dim[1];j++) {
	for (k=0;k<t->h.dim[2];k++) {
	  temp = tensor_get3d(t,i,j,k);
	  result += temp * temp;
	}
      }
    }
    break;
  }
  return sqrt(result);
}



tensor_t *inner(tensor_t *left, tensor_t *right, long k0, long k1, tensor_t *inplace) {
  long nd, i, j, k, kk;
  long d[3];
  long base = 0;
  tensor_t *result = NULL;
  long dimk, dimj, dimi;
  double sum;

  if (k0 < 0) k0 += left->h.ndim;
  if (k1 < 0) k1 += right->h.ndim;

  nd = left->h.ndim + right->h.ndim - 2;
  
  // ndim !=0 && left.dim[k0] == right.dim[k1] && nd > 0 && nd < 3

  // transform3d() k0,k1 == 0,0
  // diff() k0 = 1, k1 = 0;

  for (i=0;i<k0;i++)
    d[i] = left->h.dim[i];
  for (i=k0+1; i<left->h.ndim; i++)
    d[i-1] = left->h.dim[i];
  base = left->h.ndim-1;
  for (i=0;i<k1;i++)
    d[i+base] = right->h.dim[i];
  base--;
  for (i=k1+1;i<right->h.ndim;i++)
    d[i+base] = right->h.dim[i];

  if (!inplace) {
    switch (nd) {
    case 1:
      result = tensor_create1d(d[0],TENSOR_ZERO);
      break;
    case 2:
      result = tensor_create2d(d[0],d[1],TENSOR_ZERO);
      break;
    case 3:
      result = tensor_create3d(d[0],d[1],d[2],TENSOR_ZERO);
      break;
    }
  } else
    result = inplace;

  // following case used for transform3d()
  if ((k0 == 0) && (k1 == 0)) {
    // inner_result() from madness/tensor/tensor.cc
    dimk = left->h.dim[k0];
    dimj = right->h.stride[0];
    dimi = left->h.stride[0];
    //printf("inner: %ld %ld %ld\n", dimi,dimj,dimk);
    mTxm(dimi,dimj,dimk,result->array,left->array,right->array);
    return result;
  } 

  // seriously. this is like 1000 lines of robert code.
  // boo c++/templates/traits/python.
  // hooray c!

  for (i=0;i<left->h.dim[0];i++) {
    for (j=0;j<right->h.dim[0];j++) {
      for (k=0;k<right->h.dim[2];k++) {
	sum = 0;
	// left iterates over dim 1
	// right iterates over dim 0
	for (kk=0;kk<right->h.dim[1];kk++) {
	  sum += tensor_get2d(left,i,kk) * tensor_get3d(right,kk,j,k);
	}
	tensor_set3d(result, i, j, k, sum);
      }
    }
  }
  return result;
}


double truncate_tol(func_t *f, double tol, long level) {
  long twoton = 0;
  //  if (f->truncate_method == 0) {
  //    return tol;
  twoton = 1 << level;
  return tol/twoton;
}

#ifdef BROKEN_UNROLLED_MTXM
// madness/../tensor/mxm.h:45
void mTxm(long dimi, long dimj, long dimk, double *c, double *a, double *b) {
  long i,j,k, dimk4;
  double *ai = NULL, *p = NULL;
  double ak0i, ak1i, ak2i, ak3i, aki;
  double bk0, bk1, bk2, bk3, bk;
  /*
    c(i,j) = c(i,j) + sum(k) a(k,i)*b(k,j)
          
    where it is assumed that the last index in each array is has unit
    stride and the dimensions are as provided.
          
    i loop might be long in anticpated application
          
    4-way unrolled k loop ... empirically fastest on PIII compared to
    2/3 way unrolling (though not by much).
  */
  dimk4 = (dimk/4)*4;
  for (i=0;i<dimi;i++,c+=dimj) {
    ai = a+i;
    p = b;
    for (k=0;k<dimk4;k+=4,ai+=4*dimi,p+=4*dimj) {
      ak0i = ai[0];
      ak1i = ai[dimi];
      ak2i = ai[dimi+dimi];
      ak3i = ai[dimi+dimi+dimi];
      bk0  = p;
      bk1  = p+dimj;
      bk2  = p+dimj+dimj;
      bk3  = p+dimj+dimj+dimj;
      for (j=0;j<dimj;j++) {
	c[j] += ak0i*bk0[j] + ak1i*bk1[j] + ak2i*bk2[j] + ak3i*bk3[j];
      }
    }
    for (k=dimk4;k<dimk;k++) {
      aki = a[k*dimi+i];
      bk  = b+k*dimj;
      for (j=0;j<dimj;j++) {
	c[j] += aki*bk[j];
      }
    }
  }
}
#else
// slow, but correct and simple version
void mTxm(long dimi, long dimj, long dimk, double *c, double *a, double *b) {
  long i,j,k;
  for (k=0;k<dimk;k++) {
    for (j=0;j<dimj;j++) {
      for (i=0;i<dimi;i++) {
	c[i*dimj+j] += a[k*dimi+i]*b[k*dimj+j];
      }
    }
  }
}
#endif



// from madness3/mra/mra.py:282 (multiple verseions in file)
void fcube(func_t *f, long n, double lx, double  ly, double lz, double h, double (*fn)(double p ,double q, double r), tensor_t *fcube) {
  tensor_t *quad_x = f->quad_x;
  int i,j,k, npt;
  double x,y,z;
  
  npt = f->npt;

  for (i=0;i<npt;i++) {
    x = lx + h*tensor_get1d(quad_x,i);
    //printf("%f %f %f %f\n", x,lx,h,tensor_get1d(quad_x,i));

    for (j=0;j<npt;j++) {
      y = ly + h*tensor_get1d(quad_x,j);
      for (k=0;k<npt;k++) {
	z = lz + h*tensor_get1d(quad_x,k);
	tensor_set3d(fcube,i,j,k,fn(x,y,z));
      }	
    }
  }
}




void math_test(void) {
  tensor_t *a = tensor_create3d(2,2,2,TENSOR_NOZERO);
  tensor_t *bb = tensor_create2d(2,2,TENSOR_NOZERO);
  tensor_t *l = tensor_create2d(9,9, TENSOR_NOZERO);
  tensor_t *r = tensor_create3d(9,9,9, TENSOR_NOZERO);
  double norm;
  //int i,j,k;

  tensor_fillindex(a);
  //tensor_print(a);
  
  tensor_scale(a,2.0);
  //tensor_print(a);

  tensor_set2d(bb,0,0,sqrt(1.0));
  tensor_set2d(bb,0,1,sqrt(2.0));
  tensor_set2d(bb,1,0,sqrt(3.0));
  tensor_set2d(bb,1,1,sqrt(4.0));
  norm = normf(bb);
  printf("norm (bb) : %.10f (should be 3.16227766017)\n", norm);

  norm = normf(a);
  printf("norm (a) : %f\n", norm);

  tensor_fillindex(l);
  tensor_fillindex(r);
  tensor_scale(r,0.5);

  tensor_print(l,0);
  tensor_print(r,0);

  printf("norms: l: %f r: %f\n", normf(l), normf(r));
  
  bb = inner(l,r,1,0,NULL);
  tensor_print(bb,1);
}

