/**********************************************************/
/*                                                        */
/*    init.c                                              */
/*                                                        */
/*    author: d. brian larkins                            */
/*    id: $Id: tensor.c 618 2007-03-06 23:53:38Z dinan $  */
/*                                                        */
/**********************************************************/

#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#include "tensor.h"
#include "diffconst.h"
#include "mad3d.h"


static inline double phase(long i);

static inline double phase(long i) {
  return (i&1) ? -1.0 : 1.0;
}

/*
 * initialize block derivative operators
 */ 
void make_dc_periodic(func_t *f) {
  double iphase = 1.0;
  double jphase = 1.0;
  double gammaij, gamma;
  double Kij;
  int i,j;

  f->r0 = tensor_create2d(f->k, f->k,TENSOR_ZERO);
  f->rp = tensor_create2d(f->k, f->k,TENSOR_ZERO);
  f->rm = tensor_create2d(f->k, f->k,TENSOR_ZERO);

  for (i=0;i<f->k;i++) {
    jphase = 1.0;
    for (j=0;j<f->k;j++) {
      gammaij = sqrt((double)((2*i+1)*(2*j+1)));
      if (((i-j)>0) && (((i-j)%2)==1))
	Kij = 2.0;
      else
	Kij = 0.0;

      tensor_set2d(f->r0,i,j, 0.5*(1.0 - iphase*jphase - 2.0*Kij)*gammaij);
      tensor_set2d(f->rm,i,j, 0.5*jphase*gammaij);
      tensor_set2d(f->rp,i,j, 0.5*jphase*gammaij);
      jphase = -jphase;
    }
    iphase = -iphase;
  }

  f->rm_left  = tensor_create1d(f->k,TENSOR_ZERO);
  f->rm_right = tensor_create1d(f->k,TENSOR_ZERO);
  f->rp_left  = tensor_create1d(f->k,TENSOR_ZERO);
  f->rp_right = tensor_create1d(f->k,TENSOR_ZERO);

  iphase = 1.0;
  for (i=0;i<f->k;i++) {
    gamma = sqrt(0.5*(2*i+1));
    tensor_set1d(f->rm_left ,i, gamma);
    tensor_set1d(f->rp_right,i, gamma);
    tensor_set1d(f->rm_right,i, gamma*iphase);
    tensor_set1d(f->rp_left ,i, gamma*-iphase);
    iphase *= -1.0;
  }
}



/*
 * initialize quad_x, quad_w, quad_phi, quad_phiT, quad_phiw
 */
void init_quadrature(func_t *f) {
  double phi_norms[100];
  double phi[200];
  double nn1[100];
  double x;
  int i,j,n;
  int first_sf   = 1;
  int first_poly = 1;


  f->quad_x    = tensor_create1d(f->k,TENSOR_ZERO);
  f->quad_w    = tensor_create1d(f->k,TENSOR_ZERO);
  f->quad_phi  = tensor_create2d(f->k, f->k,TENSOR_ZERO);
  f->quad_phiw = tensor_create2d(f->k, f->k,TENSOR_ZERO);
  f->quad_phiT = tensor_create2d(f->k, f->k,TENSOR_ZERO);

  // gauss-legendre coeff points and weights
  for (i=0;i<f->k; i++) {
    tensor_set1d(f->quad_x, i, quad_points[i]);
    tensor_set1d(f->quad_w, i, quad_weights[i]);
  }

 
  for (i=0; i<f->k; i++) {         // start legendre scaling functions
    if (first_sf) {
      for (n=0;n<100;n++) 
	phi_norms[n] = sqrt(2.0*n+1.0);
      first_sf = 0;
    }

    phi[0] = 1.0;                                        // start legendre_polynomials()
    if ((f->k - 1) == 0) return;
    if (first_poly) {
      for (n=0;n<100;n++)
	nn1[n] = n/((double) (n+1));
      first_poly = 0;
    }
    x = 2.0 * tensor_get1d(f->quad_x,i) - 1;
    phi[1] = x;
    for (n=1;n<(f->k - 1);n++)
      phi[n+1] = (x*phi[n] - phi[n-1])*nn1[n] + x*phi[n]; // end legendre_polynomials

    for (n=0; n<f->k; n++)
      phi[n] = phi[n]*phi_norms[n]; // end legendre_scaling_functions
    
    for (j=0; j<f->k; j++) {
      tensor_set2d(f->quad_phi,i,j,phi[j]);
      tensor_set2d(f->quad_phiw,i,j,tensor_get1d(f->quad_w,i)*phi[j]);
    }
  }

  // transpose - by hand.
  for (i=0;i<f->k;i++) {
    for (j=0;j<f->k;j++) {
      tensor_set2d(f->quad_phiT,i,j,tensor_get2d(f->quad_phi,j,i));
    }
  }
}


/*
 *  init hg, hgT, and hgsonly
 */
void init_twoscale(func_t *f) {
  long i,j;

  f->hg      = two_scale_hg(f->k);
  f->hgT     = tensor_create2d(2*f->k,2*f->k,TENSOR_ZERO);
  f->hgsonly = tensor_create2d(f->k,2*f->k,TENSOR_ZERO);

  // hgT = hg transpose
  for (i=0;i<2*f->k;i++) {
    for (j=0;j<2*f->k;j++) {
      tensor_set2d(f->hgT,i,j,tensor_get2d(f->hg,j,i));
    }
  }

  // hgsonly == hg[1..k][1..2k]
  for (i=0;i<f->k;i++) {
    for (j=0;j<2*f->k;j++) {
      tensor_set2d(f->hgsonly,i,j,tensor_get2d(f->hg,i,j));
    }
  }
}




/*
 *  initialize two-scale relation matrix
 */
tensor_t *two_scale_hg(int k) {
  tensor_t *hg = NULL;
  double h1[9][9], g1[9][9];
  int i,j;

  if (k != 9)
    printf("K != 9. EXPECT TOTAL MELTDOWN\n");

  // h0 and g0 are in diffconst.h

  for (i=0;i<k;i++) {
    for (j=0;j<k;j++) {
      h1[i][j] = h0[i][j]*phase(i+j);
      g1[i][j] = g0[i][j]*phase(i+j+k);
    }
  }
  
  hg = tensor_create2d(2*k,2*k,TENSOR_ZERO);
  for (i=0;i<k;i++) {
    for (j=0;j<k;j++) {
      tensor_set2d(hg, i,   j,   h0[i][j]);
      tensor_set2d(hg, i,   j+k, h1[i][j]);
      tensor_set2d(hg, i+k, j,   g0[i][j]);
      tensor_set2d(hg, i+k, j+k, g1[i][j]);
    }
  }

  return hg;
}

