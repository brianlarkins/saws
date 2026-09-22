#include <vector>

#include "ring.hpp"
#include "check.hpp"

using namespace gputc;

int main() {
  for (uint32_t cap = 1; cap <= 64; cap *= 2) {
    std::vector<int> data(cap);
    RingView<int> ring{data.data(), cap};
    const uint32_t starts[] = {0u, 1u, cap - 1, cap, 3 * cap + 1, 0xFFFFFFFFu - cap, 0xFFFFFFFFu};
    for (uint32_t base : starts) {
      for (uint32_t off = 0; off < 2 * cap; ++off) {
        uint32_t pos = base + off;
        for (uint32_t n = 0; n <= cap; ++n) {
          Spans s = ring.spans(pos, n);
          CHECK(s.first.len + s.second.len == n);
          CHECK(s.first.index + s.first.len <= cap);
          CHECK(s.second.len == 0 || (s.second.index == 0 && s.first.index + s.first.len == cap));
          for (uint32_t i = 0; i < n; ++i) {
            uint32_t expect = ring.index(pos + i);
            uint32_t actual = i < s.first.len ? s.first.index + i : s.second.index + (i - s.first.len);
            CHECK(expect == actual);
          }
        }
      }
    }
  }
  std::printf("test_ring: ok\n");
}
