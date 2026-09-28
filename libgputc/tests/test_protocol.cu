// Protocol tests for libgputc (nvcc). Half-steal arithmetic runs on the host;
// ring smoke test runs on the device.

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include <cuda_runtime.h>

#include "steal_word.hpp"
#include "saws_ring.hpp"
#include "tests/check.hpp"

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
      CHECK(half_claimed_prefix(n, k) == n - half_remaining(n, k));
    }
  }
}

static void test_tiling() {
  for (uint32_t initial = 0; initial <= 200; ++initial) {
    std::vector<int> hit(initial, 0);
    uint64_t w = pack(Fields{0, false, 2, initial, 7});
    const uint32_t h = initial == 0 ? 0 : bit_width(initial - 1);
    for (uint32_t k = 0; k <= h + 2; ++k) {
      uint64_t old = w;
      w += claim_ticket_increment();
      Ticket t = interpret_claim(old);
      if (!t.valid) {
        CHECK(half_remaining(initial, k) == 0);
        continue;
      }
      for (uint32_t j = 0; j < t.n; ++j) hit[t.first + j]++;
    }
    for (uint32_t c = 0; c < initial; ++c) CHECK(hit[c] == 1);
  }
}

static void test_close_race() {
  for (int trial = 0; trial < 50; ++trial) {
    const uint32_t initial = 1 + trial % 80;
    std::atomic<uint64_t> word{pack(Fields{0, false, 1, initial, 0})};
    std::atomic<bool> go{false};
    std::vector<std::vector<Ticket>> got(4);
    std::vector<std::thread> thieves;
    for (int t = 0; t < 4; ++t) {
      thieves.emplace_back([&, t] {
        while (!go.load()) {}
        for (int i = 0; i < 100; ++i) {
          uint64_t w = word.load(std::memory_order_relaxed);
          if (!stealable(w)) continue;
          uint64_t old = word.fetch_add(claim_ticket_increment(), std::memory_order_acq_rel);
          Ticket k = interpret_claim(old);
          if (k.valid) got[t].push_back(k);
        }
      });
    }
    go = true;
    uint64_t pre = word.fetch_or(CLOSED_BIT, std::memory_order_acq_rel);
    for (auto& th : thieves) th.join();

    uint32_t sum = 0;
    std::vector<int> hit(initial, 0);
    for (auto& v : got)
      for (auto& k : v) {
        sum += k.n;
        for (uint32_t j = 0; j < k.n; ++j) hit[k.first + j]++;
      }
    CHECK(sum == taken_at_close(pre));
    for (uint32_t c = 0; c < initial; ++c) CHECK(hit[c] == (c < sum ? 1 : 0));
  }
}

struct Body {
  uint32_t id;
  uint32_t pad[3];
};

__global__ void test_ring_smoke(int* failures) {
  constexpr uint32_t C = 8;
  constexpr uint32_t Cap = 16;
  constexpr int E = 8;
  using Ring = SawsRing<Body, C, Cap, E>;

  __shared__ RingState<E> state;
  __shared__ Body storage[Ring::capacity];
  if (threadIdx.x == 0) state.init(0);
  __syncthreads();
  if (threadIdx.x != 0) return;

  Ring ring(&state, storage);
  int local_fail = 0;

  Range r{};
  if (ring.push_reserve(C, C, &r) != ReserveStatus::Ok || r.n != C) local_fail = 1;
  for (uint32_t i = 0; i < C; ++i) ring[r.pos + i].id = i;
  if (!ring.release_half()) local_fail = 1;
  uint64_t pre = 0;
  Ticket t = ring.claim(&pre);
  if (!t.valid || t.n == 0) local_fail = 1;
  ring.retire(t);
  if (!ring.acquire_half()) local_fail = 1;
  ring.reclaim();
  if (local_fail) atomicAdd(failures, 1);
}

int main() {
  test_half_arithmetic();
  test_tiling();
  test_close_race();

  int* d_fail = nullptr;
  int h_fail = 0;
  cudaMalloc(&d_fail, sizeof(int));
  cudaMemset(d_fail, 0, sizeof(int));
  test_ring_smoke<<<1, 32>>>(d_fail);
  cudaMemcpy(&h_fail, d_fail, sizeof(int), cudaMemcpyDeviceToHost);
  cudaFree(d_fail);
  CHECK(h_fail == 0);

  std::printf("test_protocol: ok\n");
  return 0;
}
