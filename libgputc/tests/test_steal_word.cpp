#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "steal_word.hpp"
#include "atomics.hpp"
#include "check.hpp"

using namespace gputc;

static uint32_t oracle_remaining(uint32_t n, uint32_t k) {
  if (n == 0) return 0;
  uint32_t rem = n;
  for (uint32_t t = 0; t < k; ++t) {
    if (rem == 0) return 0;
    rem -= (rem == 1) ? 1u : rem / 2;
  }
  return rem;
}

static void test_half_arithmetic() {
  for (uint32_t n = 0; n <= 8191; ++n) {
    const uint32_t h = n == 0 ? 0 : bit_width(n - 1);
    for (uint32_t k = 0; k <= h + 3; ++k)
      CHECK(half_remaining(n, k) == oracle_remaining(n, k));
  }
}

static void test_tiling() {
  for (uint32_t initial = 0; initial <= 150; ++initial) {
    std::vector<int> hit(initial, 0);
    uint64_t w = pack(Fields{0, false, 2, initial, 0});
    const uint32_t h = initial == 0 ? 0 : bit_width(initial - 1);
    for (uint32_t k = 0; k <= h + 2; ++k) {
      uint64_t old = w;
      w += claim_ticket_increment();
      Ticket t = interpret_claim(old);
      if (!t.valid) continue;
      for (uint32_t j = 0; j < t.n; ++j) hit[t.first + j]++;
    }
    for (uint32_t c = 0; c < initial; ++c) CHECK(hit[c] == 1);
  }
}

int main() {
  test_half_arithmetic();
  test_tiling();
  std::printf("test_steal_word: ok\n");
}
