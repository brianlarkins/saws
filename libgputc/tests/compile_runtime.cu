// Instantiates the real scheduler/kernel templates (compile-only smoke).

#include "gputc.cuh"

struct NullBody {
  uint32_t v;
};

struct CompileTask {
  using Body = NullBody;
  template<class Ctx>
  static __device__ void execute(Ctx&, const NullBody&, bool) {}
};

struct CompileCfg : gputc::DefaultConfig {
  static constexpr int warps_per_block = 2;
  static constexpr int deque_chunks = 8;
};

int main() {
  (void)&gputc::persistent_kernel<CompileTask, CompileCfg>;
  (void)&gputc::init_queues<CompileTask, CompileCfg>;
  return 0;
}
