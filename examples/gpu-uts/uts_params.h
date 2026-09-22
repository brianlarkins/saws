#pragma once

// Glue between uts.c's global parameters and the device-side tree code.

#include <cstring>

#include "uts_dev.cuh"
#include "uts.h"
#undef max
#undef min

inline UtsParams uts_params_from_globals() {
  UtsParams p{int(type), b_0, gen_mx, int(shape_fn), nonLeafBF, nonLeafProb, shiftDepth, computeGranularity, {}};
  for (int d = 0; d < uts::GEO_DEPTHS; ++d) p.geo_log_q[d] = uts::geo_log_q(d, p);
  return p;
}

inline UtsNode uts_root() {
  Node root;
  uts_initRoot(&root, type);
  root.numChildren = uts_numChildren(&root);
  UtsNode r;
  r.type = root.type;
  r.height = root.height;
  r.numChildren = root.numChildren;
  std::memcpy(r.state, root.state.state, sizeof(r.state));
  return r;
}
