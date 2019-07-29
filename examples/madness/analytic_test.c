#include <stdio.h>
#include <stdlib.h>

#include "mad_analytics.h"

int main (int argc, char **argv) {
  int i, j, k;
  int npt;
  double (*afcn)(double x, double y, double z); 

  afcn = sphere;
  npt = 10;

  for (i = 0; i <= npt; i++)
    for (j = 0; j <= npt; j++)
      for (k = 0; k <= npt; k++) {
        double v = afcn(i/(double)npt, j/(double)npt, k/(double)npt);
        printf("(%0.4f, %0.4f, %0.4f) -- Analytic=%8.5f\n", i/(double)npt,
            j/(double)npt, k/(double)npt, v);
      }
}

