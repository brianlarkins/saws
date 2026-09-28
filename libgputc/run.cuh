#pragma once

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <cuda_runtime.h>

#include "kernel.cuh"

#define GPUTC_CUDA_CHECK(call)                                              \
  do {                                                                      \
    cudaError_t gputc_err_ = (call);                                        \
    if (gputc_err_ != cudaSuccess) {                                        \
      std::fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #call,    \
                   cudaGetErrorString(gputc_err_));                         \
      std::abort();                                                         \
    }                                                                       \
  } while (0)

namespace gputc {

template<class Task, class Cfg>
static size_t deque_bytes_per_worker() {
  return size_t(Cfg::deque_chunks) * Cfg::chunk * sizeof(typename Task::Body);
}

template<class Task, class Cfg>
static bool checked_worker_bytes(int workers, size_t* out) {
  if (workers <= 0 || !valid_worker_count(workers)) return false;
  const size_t per = deque_bytes_per_worker<Task, Cfg>();
  if (per == 0 || per > (size_t(1) << 30)) return false;
  const size_t total = per * size_t(workers);
  if (total / per != size_t(workers)) return false;
  *out = total;
  return true;
}

template<class Task, class Cfg = DefaultConfig>
Stats run(const typename Task::Body& root, const Policy& policy = Policy{},
          const LaunchOptions& launch = LaunchOptions{}) {
  using Body  = typename Task::Body;
  using State = RingState<Cfg::epochs>;

  const int warps_per_block = Cfg::warps_per_block;
  const int block_threads   = warps_per_block * Cfg::chunk;

  int device = 0;
  GPUTC_CUDA_CHECK(cudaGetDevice(&device));

  int cooperative = 0;
  GPUTC_CUDA_CHECK(cudaDeviceGetAttribute(&cooperative, cudaDevAttrCooperativeLaunch, device));
  if (!cooperative) {
    std::fprintf(stderr, "gputc: device does not support cooperative launch\n");
    std::abort();
  }

  int num_sm = 0;
  GPUTC_CUDA_CHECK(cudaDeviceGetAttribute(&num_sm, cudaDevAttrMultiProcessorCount, device));

  auto kernel = persistent_kernel<Task, Cfg>;
  int max_blocks_per_sm = 0;
  GPUTC_CUDA_CHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &max_blocks_per_sm, kernel, block_threads, 0));
  const int max_grid = max_blocks_per_sm * num_sm;
  if (max_grid <= 0) {
    std::fprintf(stderr, "gputc: occupancy API returned no launchable blocks\n");
    std::abort();
  }

  int blocks = launch.blocks > 0 ? launch.blocks : max_grid;
  if (launch.blocks > 0 && blocks > max_grid) {
    std::fprintf(stderr,
                 "gputc: requested %d blocks exceeds cooperative limit %d for %d warps/block\n",
                 launch.blocks, max_grid, warps_per_block);
    std::abort();
  }

  const int workers = blocks * warps_per_block;
  if (!valid_worker_count(workers)) {
    std::fprintf(stderr, "gputc: worker count %d exceeds steal-counter guard contract\n", workers);
    std::abort();
  }

  size_t body_bytes = 0;
  if (!checked_worker_bytes<Task, Cfg>(workers, &body_bytes)) {
    std::fprintf(stderr, "gputc: deque allocation size overflow\n");
    std::abort();
  }

  RunControl h_run{};
  RunControl* d_run = nullptr;
  unsigned long long* d_spawned = nullptr;
  unsigned long long* d_executed = nullptr;
  State* d_states = nullptr;
  Body* d_bodies = nullptr;
  WarpCounters* d_out = nullptr;

  GPUTC_CUDA_CHECK(cudaMalloc(&d_run, sizeof(RunControl)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_spawned, size_t(workers) * sizeof(unsigned long long)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_executed, size_t(workers) * sizeof(unsigned long long)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_states, size_t(workers) * sizeof(State)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_bodies, body_bytes));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_out, size_t(workers) * sizeof(WarpCounters)));
  GPUTC_CUDA_CHECK(cudaMemcpy(d_run, &h_run, sizeof(RunControl), cudaMemcpyHostToDevice));
  GPUTC_CUDA_CHECK(cudaMemset(d_out, 0, size_t(workers) * sizeof(WarpCounters)));

  KernelArgs<Task, Cfg> args{d_run, d_spawned, d_executed, d_states, d_bodies, d_out, workers, policy};

  cudaEvent_t ev_init_start, ev_init_stop, ev_start, ev_stop;
  GPUTC_CUDA_CHECK(cudaEventCreate(&ev_init_start));
  GPUTC_CUDA_CHECK(cudaEventCreate(&ev_init_stop));
  GPUTC_CUDA_CHECK(cudaEventCreate(&ev_start));
  GPUTC_CUDA_CHECK(cudaEventCreate(&ev_stop));

  GPUTC_CUDA_CHECK(cudaEventRecord(ev_init_start));
  init_queues<Task, Cfg><<<blocks, block_threads>>>(args, root);
  GPUTC_CUDA_CHECK(cudaGetLastError());
  GPUTC_CUDA_CHECK(cudaEventRecord(ev_init_stop));

  void* coop_args[] = {&args};
  GPUTC_CUDA_CHECK(cudaEventRecord(ev_start));
  GPUTC_CUDA_CHECK(cudaLaunchCooperativeKernel((void*)kernel, blocks, block_threads, coop_args));
  GPUTC_CUDA_CHECK(cudaGetLastError());
  GPUTC_CUDA_CHECK(cudaEventRecord(ev_stop));
  GPUTC_CUDA_CHECK(cudaEventSynchronize(ev_stop));

  float init_ms = 0, ms = 0;
  GPUTC_CUDA_CHECK(cudaEventElapsedTime(&init_ms, ev_init_start, ev_init_stop));
  GPUTC_CUDA_CHECK(cudaEventElapsedTime(&ms, ev_start, ev_stop));

  Stats stats{};
  stats.shape = RunShape{blocks, warps_per_block, workers, Cfg::chunk, Cfg::deque_chunks,
                         Cfg::timing, deque_bytes_per_worker<Task, Cfg>()};
  stats.seconds = ms / 1000.0;
  stats.max_blocks = unsigned(max_grid);
  stats.per_warp.resize(workers);

  GPUTC_CUDA_CHECK(cudaMemcpy(stats.per_warp.data(), d_out, size_t(workers) * sizeof(WarpCounters),
                              cudaMemcpyDeviceToHost));

  RunControl h_final{};
  GPUTC_CUDA_CHECK(cudaMemcpy(&h_final, d_run, sizeof(RunControl), cudaMemcpyDeviceToHost));
  stats.error_code = h_final.error_code;
  stats.error_worker = h_final.error_worker;

  std::vector<unsigned long long> h_spawned(workers), h_executed(workers);
  GPUTC_CUDA_CHECK(cudaMemcpy(h_spawned.data(), d_spawned, size_t(workers) * sizeof(unsigned long long),
                              cudaMemcpyDeviceToHost));
  GPUTC_CUDA_CHECK(cudaMemcpy(h_executed.data(), d_executed, size_t(workers) * sizeof(unsigned long long),
                              cudaMemcpyDeviceToHost));
  for (int i = 0; i < workers; ++i) {
    stats.global_spawned += h_spawned[i];
    stats.global_executed += h_executed[i];
  }

  cudaFuncAttributes attr{};
  if (cudaFuncGetAttributes(&attr, kernel) == cudaSuccess) {
    stats.registers = unsigned(attr.numRegs);
    stats.static_smem = unsigned(attr.sharedSizeBytes);
    stats.dynamic_smem = unsigned(attr.sharedSizeBytes);
  }

  stats.reduce();

  std::fprintf(stderr, "gputc: init %.3f ms, workers %d, deque %zu bytes each (total %.2f MiB)\n",
               init_ms, workers, stats.shape.deque_bytes, double(body_bytes) / (1024.0 * 1024.0));

  cudaEventDestroy(ev_init_start);
  cudaEventDestroy(ev_init_stop);
  cudaEventDestroy(ev_start);
  cudaEventDestroy(ev_stop);
  cudaFree(d_out);
  cudaFree(d_bodies);
  cudaFree(d_states);
  cudaFree(d_executed);
  cudaFree(d_spawned);
  cudaFree(d_run);
  return stats;
}

} // namespace gputc
