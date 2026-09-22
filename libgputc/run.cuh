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

// Run a task tree to completion from one root task on the current device.
template<class Task, class Cfg = DefaultConfig>
Stats run(const typename Task::Body& root, const Policy& policy = Policy{}) {
  using Body    = typename Task::Body;
  using Control = GlobalControl<Cfg>;
  const size_t global_slots = size_t(Cfg::global_chunks) * Cfg::chunk;

  Control init{};
  init.ring.init();

  Control*      d_control = nullptr;
  Body*         d_slots   = nullptr;
  WarpCounters* d_out     = nullptr;
  GPUTC_CUDA_CHECK(cudaMalloc(&d_control, sizeof(Control)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_slots, global_slots * sizeof(Body)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_out, Cfg::warps * sizeof(WarpCounters)));
  GPUTC_CUDA_CHECK(cudaMemcpy(d_control, &init, sizeof(Control), cudaMemcpyHostToDevice));
  GPUTC_CUDA_CHECK(cudaMemset(d_out, 0, Cfg::warps * sizeof(WarpCounters)));

  const size_t smem = deque_shared_bytes<Task, Cfg>();
  auto kernel = persistent_kernel<Task, Cfg>;
  GPUTC_CUDA_CHECK(cudaFuncSetAttribute(kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, int(smem)));

  KernelArgs<Task, Cfg> args{d_control, d_slots, d_out, policy, root};

  cudaEvent_t start, stop;
  GPUTC_CUDA_CHECK(cudaEventCreate(&start));
  GPUTC_CUDA_CHECK(cudaEventCreate(&stop));
  GPUTC_CUDA_CHECK(cudaEventRecord(start));
  kernel<<<1, Cfg::warps * warp::SIZE, smem>>>(args);
  GPUTC_CUDA_CHECK(cudaGetLastError());
  GPUTC_CUDA_CHECK(cudaEventRecord(stop));
  GPUTC_CUDA_CHECK(cudaEventSynchronize(stop));

  float ms = 0;
  GPUTC_CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));

  Stats stats{};
  stats.shape = RunShape{Cfg::warps, Cfg::chunk, Cfg::warp_chunks, Cfg::global_chunks, Cfg::timing};
  stats.seconds = ms / 1000.0;
  stats.per_warp.resize(Cfg::warps);
  GPUTC_CUDA_CHECK(cudaMemcpy(stats.per_warp.data(), d_out, Cfg::warps * sizeof(WarpCounters), cudaMemcpyDeviceToHost));
  Control final_control;
  GPUTC_CUDA_CHECK(cudaMemcpy(&final_control, d_control, sizeof(Control), cudaMemcpyDeviceToHost));
  stats.global_peak = final_control.peak;
  stats.reduce();

  cudaEventDestroy(start);
  cudaEventDestroy(stop);
  cudaFree(d_out);
  cudaFree(d_slots);
  cudaFree(d_control);
  return stats;
}

} // namespace gputc
