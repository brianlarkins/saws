// Protocol + runtime compile tests (nvcc + GPU for device cases).

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include <cuda_runtime.h>

#include "steal_word.hpp"
#include "saws_ring.hpp"
#include "tests/check.hpp"

#define CUDA_CHECK(call)                                                         \
  do {                                                                           \
    cudaError_t err = (call);                                                    \
    if (err != cudaSuccess) {                                                    \
      std::fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #call,         \
                   cudaGetErrorString(err));                                       \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace gputc;

static uint32_t oracle_remaining(uint32_t n, uint32_t k) {
  if (n == 0) return 0;
  uint32_t rem = n;
  for (uint32_t t = 0; t < k; ++t) {
    if (rem == 0) return 0;
    const uint32_t share = (rem == 1) ? 1u : rem / 2;
    rem -= share;
  }
  return rem;
}

static void test_half_arithmetic() {
  for (uint32_t n = 0; n <= 8191; ++n) {
    const uint32_t h = n == 0 ? 0 : bit_width(n - 1);
    for (uint32_t k = 0; k <= h + 3; ++k) {
      CHECK(half_remaining(n, k) == oracle_remaining(n, k));
      ChunkRange cr = half_claim(n, k);
      const uint32_t before = half_remaining(n, k);
      const uint32_t after  = half_remaining(n, k + 1);
      CHECK(cr.first == n - before);
      CHECK(cr.count == before - after);
    }
  }
}

static void test_close_race() {
  for (int trial = 0; trial < 30; ++trial) {
    const uint32_t initial = 1 + trial % 80;
    std::atomic<uint64_t> word{pack(Fields{0, false, 1, initial, 0})};
    std::atomic<bool> go{false};
    std::vector<std::thread> thieves;
    for (int t = 0; t < 4; ++t) {
      thieves.emplace_back([&] {
        while (!go.load()) {}
        for (int i = 0; i < 80; ++i) {
          uint64_t w = word.load(std::memory_order_relaxed);
          if (!stealable(w)) continue;
          uint64_t old = word.fetch_add(claim_ticket_increment(), std::memory_order_acq_rel);
          (void)interpret_claim(old);
        }
      });
    }
    go = true;
    uint64_t pre = word.fetch_or(CLOSED_BIT, std::memory_order_acq_rel);
    for (auto& th : thieves) th.join();
    (void)pre;
  }
}

struct Body {
  uint32_t id;
  uint32_t pad[3];
};

struct RingSmokeDev {
  RingState<8> state;
  Body         slots[16 * 8];
};

// failure_code: 1=push 2=release 3=claim 4=acquire 5=half-acquire 6=state
__global__ void test_ring_smoke(RingSmokeDev* dev, int* failure_code) {
  constexpr uint32_t C = 8;
  constexpr uint32_t Cap = 16;
  constexpr int E = 8;
  using Ring = SawsRing<Body, DeviceScope, C, Cap, E>;

  if (threadIdx.x == 0) dev->state.init(0);
  __syncthreads();
  if (threadIdx.x != 0) return;

  Ring ring(&dev->state, dev->slots);

  Range r{};
  if (ring.push_reserve(2 * C, 2 * C, &r) != ReserveStatus::Ok || r.n != 2 * C) {
    *failure_code = 1;
    return;
  }
  if (ring.release_half() != 1) {
    *failure_code = 2;
    return;
  }
  uint64_t pre = 0;
  Ticket t = ring.claim(&pre);
  if (!t.valid || t.n == 0) {
    *failure_code = 3;
    return;
  }
  ring.retire(t);
  if (ring.acquire_half_status() != AcquireStatus::HasPrivate) {
    *failure_code = 4;
    return;
  }

  dev->state.init(0);
  if (ring.push_reserve(C, C, &r) != ReserveStatus::Ok || r.n != C) {
    *failure_code = 5;
    return;
  }
  if (ring.release_half() != 1 || ring.private_size() != 0) {
    *failure_code = 5;
    return;
  }
  if (ring.acquire_half_status() != AcquireStatus::HasPrivate || ring.private_size() == 0) {
    *failure_code = 5;
    return;
  }
}

int main() {
  test_half_arithmetic();
  test_close_race();

  RingSmokeDev* d_buf = nullptr;
  int* d_fail = nullptr;
  int h_fail = -1;
  CUDA_CHECK(cudaMalloc(&d_buf, sizeof(RingSmokeDev)));
  CUDA_CHECK(cudaMalloc(&d_fail, sizeof(int)));
  CUDA_CHECK(cudaMemset(d_fail, 0, sizeof(int)));
  test_ring_smoke<<<1, 32>>>(d_buf, d_fail);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());
  CUDA_CHECK(cudaMemcpy(&h_fail, d_fail, sizeof(int), cudaMemcpyDeviceToHost));
  if (h_fail != 0)
    std::fprintf(stderr, "test_ring_smoke failed with code %d "
                 "(1=push 2=release 3=claim 4=acquire 5=half-acquire 6=state)\n", h_fail);
  CUDA_CHECK(cudaFree(d_fail));
  CUDA_CHECK(cudaFree(d_buf));
  CHECK(h_fail == 0);

  std::printf("test_protocol: ok\n");
  return 0;
}
