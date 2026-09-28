// UTS on GPU SAWS V2 (libgputc): cooperative multi-block grid, global deques.
//
//   source ../uts/sample_trees.sh && ./gpu-uts $T1
//
// Extra flags:
//   -S 1       per-worker statistics table
//   -W warps   warps per block (default 8)
//   -B blocks  block count (0 = occupancy auto, default 0)

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gputc.cuh"
#include "uts_params.h"

static int stats_per_warp = 0;
static int block_warps    = 8;
static int grid_blocks    = 0;

extern "C" {
char* impl_getName() { return (char*)"GPU SAWS V2 (libgputc, global deques)"; }
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
  if (std::strcmp(param, "-B") == 0) {
    grid_blocks = std::atoi(value);
    return 0;
  }
  return 1;
}
void  impl_helpMessage() {
  std::printf("   -S  int   nonzero to print per-worker statistics\n");
  std::printf("   -W  int   warps per block (e.g. 1, 4, 8, 16, 32)\n");
  std::printf("   -B  int   blocks (0 = automatic cooperative grid)\n");
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

#ifndef GPUTC_DEQUE_CHUNKS
#define GPUTC_DEQUE_CHUNKS 256
#endif

template<int Warps>
struct UtsConfig : gputc::DefaultConfig {
  static constexpr int warps_per_block = Warps;
  static constexpr int deque_chunks    = GPUTC_DEQUE_CHUNKS;
  static constexpr int epochs          = 8;
  static constexpr bool timing         = false;
};

static gputc::Stats run_tree(const UtsNode& root) {
  gputc::LaunchOptions launch{};
  launch.blocks = grid_blocks;
  gputc::Policy policy{};
  switch (block_warps) {
    case 1:  return gputc::run<UtsTask, UtsConfig<1>>(root, policy, launch);
    case 4:  return gputc::run<UtsTask, UtsConfig<4>>(root, policy, launch);
    case 8:  return gputc::run<UtsTask, UtsConfig<8>>(root, policy, launch);
    case 16: return gputc::run<UtsTask, UtsConfig<16>>(root, policy, launch);
    case 32: return gputc::run<UtsTask, UtsConfig<32>>(root, policy, launch);
  }
  std::fprintf(stderr, "gpu-uts: -W must be 1, 4, 8, 16 or 32\n");
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
