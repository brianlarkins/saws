#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI           3.14159265358979323846
#endif

// Estimate for a wave function: Sum of Gaussians
double wavefcn(double x, double y, double z) {
  double alpha = 5.0;
  x = x-0.5;
  y = y-0.5;
  z = z-0.5;
  return exp(-alpha*(x*x+y*y+z*z))*5.0;
}


// Potential function for hydrogen atom: produces a singularity at (xx,yy,zz)
double hydrogen(double x, double y, double z) {
  const double xx = 0.5;
  const double yy = 0.5;
  const double zz = 0.5;

  double dx = x - xx;
  double dy = y - yy;
  double dz = z - zz;
  
  return 1/sqrt(dx*dx + dy*dy + dz*dz);
}


// Valence electron state for metals
double metal(double x, double y, double z) {
  const double n = 50;  // <50, 10> 2,394,697 Nodes
  const double a = 1.0;
  return a * cos(n*M_PI*x) * cos(n*M_PI*y) * cos(n*M_PI*z);
}


// Find the net Coulomb potential of an N x N x N lattice of hydrogen atoms
// regularly spaced in an dim x dim x dim volume.  This is a large problem and
// should result in very deep trees around the singularities where each atom is
// placed on the lattice.
double lattice(double x, double y, double z) {
  const double dim = 10.0;
  const int    N   = 8;
  double dx2[N], dy2[N], dz2[N];
  double sum = 0.0;
  int i, j, k;

  for (i = 0; i < N; i++) {
    const double dx = dim*(x-i/(double)(N-1));
    const double dy = dim*(y-i/(double)(N-1));
    const double dz = dim*(z-i/(double)(N-1));
    
    dx2[i] = dx*dx;
    dy2[i] = dy*dy;
    dz2[i] = dz*dz;
  }

  // Gather 1/r for each point on the lattice
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      for (k = 0; k < N; k++)
        sum += 1.0/sqrt(dx2[i] + dy2[j] + dz2[k]);

  return sum;
}


// Soft particle simulation problem for Argon:
//
// If the locations of the particles were random this would be a particle
// insertion problem.  We'd be asking: What is the most likely place to put the
// next particle in the lattice?
//
// Find the net lennard-jones potential of an N x N x N lattice of regularly
// spaced water molecules in a dim x dim x dim volume.
double lj_lattice(double x, double y, double z) {
  const int    N     = 8;
  const double dim   = 10.0 /* nm */;
  const double sigma = 0.3405 /* nm */; // Argon
  const double eps   = 0.9960388 /* kJ/mol */;
  const double V_rc  = -0.0163*eps;
  double dx2[N], dy2[N], dz2[N];
  double sum = 0.0;
  int i, j, k;

  for (i = 0; i < N; i++) {
    const double dx = dim*(x-i/(double)(N-1));
    const double dy = dim*(y-i/(double)(N-1));
    const double dz = dim*(z-i/(double)(N-1));
    
    dx2[i] = dx*dx;
    dy2[i] = dy*dy;
    dz2[i] = dz*dz;
  }

  // Gather the L-J contribution from each molecule in the lattice
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      for (k = 0; k < N; k++) {
        const double r = sqrt(dx2[i] + dy2[j] + dz2[k]);
        sum += (r > 2.5*sigma) ? 0 : 4.0*eps * (pow(sigma/r, 12.0) - pow(sigma/r, 6.0)) - V_rc;
      }
        

  return sum;
}


// The Great Gaussian sphere.
double sphere(double x, double y, double z) {
  const double a   = 1000.0;
  const double s2  = 0.01;
  const double mu  = 0.25;
 
  const double sx  = 0.5; // Center of the sphere
  const double sy  = 0.5;
  const double sz  = 0.5;

  const double dx = x-sx; // Distance from the center
  const double dy = y-sy;
  const double dz = z-sz;

  const double r  = sqrt(dx*dx + dy*dy + dz*dz);

  return a*exp(-((r-mu)*(r-mu))/(2.0*s2)); // Project Gaussian around the radius

  //return fabs(r-mu) < 0.1 ? 10 : 0; // Step function -- discontinuous
  
  //return 1/(10.0*fabs(r-mu)); // 1/r on the surface of the sphere.  Note: this makes
                                // the surface of the sphere a singularity which, in
                                // practice, is problematic.
}

