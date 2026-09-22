// UTS on GPU SAWS (libgputc V1): one persistent block, warp deques in shared
// memory, one global ring. Takes the same arguments as the CPU UTS drivers:
//
//   source ../uts/sample_trees.sh && ./gpu-uts $T1

#include <cstdio>
#include <cstdlib>

#include "gputc.cuh"
#include "uts_params.h"

extern "C" {
char* impl_getName() { return (char*)"GPU SAWS (libgputc, single block)"; }
int   impl_paramsToStr(char* buf, int ind) {
  return ind + std::sprintf(buf + ind, "Execution strategy:  %s\n", impl_getName());
}
int   impl_parseParam(char*, char*) { return 1; }
void  impl_helpMessage() { std::printf("   none.\n"); }
void  impl_abort(int err) { std::exit(err); }
}

__constant__ UtsParams d_params;
__device__ unsigned long long d_leaves;
__device__ int d_max_depth;

struct UtsTask {
  using Body = UtsNode;

  template<class Ctx>
  static __device__ void execute(Ctx& ctx, const UtsNode& node, bool valid) {
    int mine = valid ? node.numChildren : 0;
    int most = ctx.warp_max(mine);
    for (int i = 0; i < most; ++i) {
      bool make = i < mine;
      UtsNode child{};
      if (make) child = uts::make_child(node, i, d_params);
      ctx.spawn(child, make);
    }

    unsigned leaves = __ballot_sync(gputc::warp::FULL, valid && node.numChildren == 0);
    int depth = ctx.warp_max(valid ? node.height : 0);
    if (ctx.lane() == 0) {
      if (leaves) atomicAdd(&d_leaves, (unsigned long long)__popc(leaves));
      atomicMax(&d_max_depth, depth);
    }
  }
};

int main(int argc, char** argv) {
  uts_parseParams(argc, argv);
  uts_printParams();

  const UtsParams params = uts_params_from_globals();
  const unsigned long long zero = 0;
  const int zero_depth = 0;
  GPUTC_CUDA_CHECK(cudaMemcpyToSymbol(d_params, &params, sizeof(params)));
  GPUTC_CUDA_CHECK(cudaMemcpyToSymbol(d_leaves, &zero, sizeof(zero)));
  GPUTC_CUDA_CHECK(cudaMemcpyToSymbol(d_max_depth, &zero_depth, sizeof(zero_depth)));

  gputc::Stats s = gputc::run<UtsTask>(uts_root());

  unsigned long long leaves = 0;
  int depth = 0;
  GPUTC_CUDA_CHECK(cudaMemcpyFromSymbol(&leaves, d_leaves, sizeof(leaves)));
  GPUTC_CUDA_CHECK(cudaMemcpyFromSymbol(&depth, d_max_depth, sizeof(depth)));

  uts_showStats(1, 0, s.seconds, s.executed, leaves, (counter_t)depth);
  std::printf("gputc: spawned %llu, executed %llu: %s\n", s.spawned, s.executed,
              s.spawned == s.executed ? "conserved" : "MISMATCH");
  std::printf("gputc: steals sibling %llu, global %llu, misses %llu; releases %llu, reacquires %llu, promotes %llu\n",
              s.steals_sibling, s.steals_global, s.steal_misses, s.releases, s.reacquires, s.promotes);
  return s.spawned == s.executed ? 0 : 1;
}
