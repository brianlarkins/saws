// Host check of uts_dev.cuh against the reference UTS code: the SHA-1 RNG and
// child rules must match exactly, and a sequential traversal with the ported
// code reproduces the tree statistics in ../uts/sample_trees.sh.
//
//   ./test-uts-dev $T1     ->  Tree size = 4130071, tree depth = 10, ...

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "uts_params.h"

extern "C" {
char* impl_getName() { return (char*)"uts_dev.cuh host check"; }
int   impl_paramsToStr(char* buf, int ind) { return ind; }
int   impl_parseParam(char*, char*) { return 1; }
void  impl_helpMessage() { std::printf("   none.\n"); }
void  impl_abort(int err) { std::exit(err); }
}

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static void check_rng() {
  std::mt19937 gen(5);
  for (int i = 0; i < 20000; ++i) {
    int seed = int(gen() & 0x7fffffff);
    uint8_t a[20], b[20];
    rng_init(a, seed);
    uts::rng_init(b, seed);
    for (int k = 0; k < 20; ++k) CHECK(a[k] == b[k]);
    int spawn = int(gen() % 1000);
    uint8_t ca[20], cb[20];
    rng_spawn(a, ca, spawn);
    uts::rng_spawn(b, cb, spawn);
    for (int k = 0; k < 20; ++k) CHECK(ca[k] == cb[k]);
    CHECK(rng_rand(ca) == uts::rng_rand(cb));
  }
}

int main(int argc, char** argv) {
  uts_parseParams(argc, argv);
  uts_printParams();
  check_rng();

  const UtsParams p = uts_params_from_globals();
  std::vector<UtsNode> stack{uts_root()};
  unsigned long long nodes = 0, leaves = 0, depth = 0;
  while (!stack.empty()) {
    UtsNode n = stack.back();
    stack.pop_back();
    ++nodes;
    if (n.numChildren == 0) ++leaves;
    if ((unsigned long long)n.height > depth) depth = n.height;
    for (int i = 0; i < n.numChildren; ++i) {
      UtsNode c = uts::make_child(n, i, p);

      Node ref;
      ref.type = c.type;
      ref.height = c.height;
      std::memcpy(ref.state.state, c.state, 20);
      CHECK(uts_childType(&ref) == uts::child_type(c, p));
      if (nodes < 100000) CHECK(uts_numChildren(&ref) == c.numChildren);

      stack.push_back(c);
    }
  }
  uts_showStats(1, 0, 1.0, nodes, leaves, depth);
  return 0;
}
