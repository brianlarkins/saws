// Sequential reference: a depth-first traversal on one CPU core with the same
// tree code as the GPU kernel (uts_dev.cuh), for a nodes/s baseline.
//
//   ./uts-seq $T1

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "uts_params.h"

extern "C" {
char* impl_getName() { return (char*)"sequential, uts_dev.cuh tree code"; }
int   impl_paramsToStr(char* buf, int ind) {
  return ind + std::sprintf(buf + ind, "Execution strategy:  %s\n", impl_getName());
}
int   impl_parseParam(char*, char*) { return 1; }
void  impl_helpMessage() { std::printf("   none.\n"); }
void  impl_abort(int err) { std::exit(err); }
}

int main(int argc, char** argv) {
  uts_parseParams(argc, argv);
  uts_printParams();

  const UtsParams p = uts_params_from_globals();
  auto t0 = std::chrono::steady_clock::now();
  std::vector<UtsNode> stack{uts_root()};
  unsigned long long nodes = 0, leaves = 0, depth = 0;
  while (!stack.empty()) {
    UtsNode n = stack.back();
    stack.pop_back();
    ++nodes;
    if (n.numChildren == 0) ++leaves;
    if ((unsigned long long)n.height > depth) depth = n.height;
    for (int i = 0; i < n.numChildren; ++i) stack.push_back(uts::make_child(n, i, p));
  }
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  uts_showStats(1, 0, seconds, nodes, leaves, depth);
  return 0;
}
