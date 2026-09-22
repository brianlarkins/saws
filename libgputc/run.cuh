#pragma once

#include <cstdio>
#include <cstdlib>

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
  using Body  = typename Task::Body;
  using State = RingState<Cfg::epochs>;
  const size_t global_slots = size_t(Cfg::global_chunks) * Cfg::chunk;

  State init;
  init.init();

  State*    d_state = nullptr;
  Body*     d_slots = nullptr;
  uint32_t* d_lock  = nullptr;
  Counters* d_out   = nullptr;
  GPUTC_CUDA_CHECK(cudaMalloc(&d_state, sizeof(State)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_slots, global_slots * sizeof(Body)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_lock, sizeof(uint32_t)));
  GPUTC_CUDA_CHECK(cudaMalloc(&d_out, sizeof(Counters)));
  GPUTC_CUDA_CHECK(cudaMemcpy(d_state, &init, sizeof(State), cudaMemcpyHostToDevice));
  GPUTC_CUDA_CHECK(cudaMemset(d_lock, 0, sizeof(uint32_t)));
  GPUTC_CUDA_CHECK(cudaMemset(d_out, 0, sizeof(Counters)));

  const size_t smem = deque_shared_bytes<Task, Cfg>();
  auto kernel = persistent_kernel<Task, Cfg>;
  GPUTC_CUDA_CHECK(cudaFuncSetAttribute(kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, int(smem)));

  KernelArgs<Task, Cfg> args{GlobalRef<Body, Cfg>{d_state, d_slots, d_lock}, d_out, policy, root};

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
  GPUTC_CUDA_CHECK(cudaMemcpy(static_cast<Counters*>(&stats), d_out, sizeof(Counters), cudaMemcpyDeviceToHost));
  stats.seconds = ms / 1000.0;

  cudaEventDestroy(start);
  cudaEventDestroy(stop);
  cudaFree(d_out);
  cudaFree(d_lock);
  cudaFree(d_slots);
  cudaFree(d_state);
  return stats;
}

} // namespace gputc
