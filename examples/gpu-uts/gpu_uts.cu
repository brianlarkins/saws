// UTS on GPU SAWS (libgputc V1): one persistent block, warp deques in shared
// memory, one global ring. Takes the same arguments as the CPU UTS drivers:
//
//   source ../uts/sample_trees.sh && ./gpu-uts $T1
//
// Extra flags:
//   -S 1       print a per-warp breakdown after the summary
//   -W warps   warps in the block: 8 (default), 16 or 32

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gputc.cuh"
#include "uts_params.h"

static int stats_per_warp = 0;
static int block_warps = 8;

extern "C" {
char* impl_getName() { return (char*)"GPU SAWS (libgputc, single block)"; }
int   impl_paramsToStr(char* buf, int ind) {
  return ind + std::sprintf(buf + ind, "Execution strategy:  %s\n", impl_getName());
}
int   impl_parseParam(char* param, char* value) {
  if (std::strcmp(param, "-S") == 0) {
    stats_per_warp = std::atoi(value);
    return 0;
  }
  if (std::strcmp(param, "-W") == 0) {
    block_warps = std::atoi(value);
    return 0;
  }
  return 1;
}
void  impl_helpMessage() {
  std::printf("   -S  int   nonzero to print per-warp statistics\n");
  std::printf("   -W  int   warps in the block: 8, 16 or 32\n");
}
void  impl_abort(int err) { std::exit(err); }
}

__constant__ UtsParams d_params;
__device__ unsigned long long d_leaves;
__device__ int d_max_depth;

struct UtsTask {
  using Body = UtsNode;

  template<class Ctx>
  static __device__ void execute(Ctx& ctx, const UtsNode& node, bool valid) {
    ctx.spawn_n(valid ? node.numChildren : 0, node,
                [](const UtsNode& parent, int i) { return uts::make_child(parent, i, d_params); });

    unsigned leaves = __ballot_sync(gputc::warp::FULL, valid && node.numChildren == 0);
    int depth = ctx.warp_max(valid ? node.height : 0);
    if (ctx.lane() == 0) {
      if (leaves) atomicAdd(&d_leaves, (unsigned long long)__popc(leaves));
      atomicMax(&d_max_depth, depth);
    }
  }
};

// Warp deques share about 96 KB of shared memory (sm_86 allows 99 KB per
// block), so more warps means shallower deques.
template<int Warps, int DequeChunks>
struct UtsConfig : gputc::DefaultConfig {
  static constexpr int warps       = Warps;
  static constexpr int warp_chunks = DequeChunks;
  static constexpr int warp_epochs = DequeChunks < 8 ? 4 : 8;
};

static gputc::Stats run_tree(const UtsNode& root) {
  switch (block_warps) {
    case 8:  return gputc::run<UtsTask, UtsConfig<8, 8>>(root);
    case 16: return gputc::run<UtsTask, UtsConfig<16, 4>>(root);
    case 32: return gputc::run<UtsTask, UtsConfig<32, 2>>(root);
  }
  std::fprintf(stderr, "gpu-uts: -W must be 8, 16 or 32\n");
  std::exit(4);
}

int main(int argc, char** argv) {
  uts_parseParams(argc, argv);
  uts_printParams();

  const UtsParams params = uts_params_from_globals();
  const unsigned long long zero = 0;
  const int zero_depth = 0;
  GPUTC_CUDA_CHECK(cudaMemcpyToSymbol(d_params, &params, sizeof(params)));
  GPUTC_CUDA_CHECK(cudaMemcpyToSymbol(d_leaves, &zero, sizeof(zero)));
  GPUTC_CUDA_CHECK(cudaMemcpyToSymbol(d_max_depth, &zero_depth, sizeof(zero_depth)));

  gputc::Stats s = run_tree(uts_root());

  unsigned long long leaves = 0;
  int depth = 0;
  GPUTC_CUDA_CHECK(cudaMemcpyFromSymbol(&leaves, d_leaves, sizeof(leaves)));
  GPUTC_CUDA_CHECK(cudaMemcpyFromSymbol(&depth, d_max_depth, sizeof(depth)));

  uts_showStats(1, 0, s.seconds, s.total.tasks, leaves, (counter_t)depth);
  s.print(stdout, stats_per_warp != 0);
  s.print_kv(stdout);
  return s.conserved() ? 0 : 1;
}
