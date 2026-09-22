#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "steal_word.hpp"
#include "atomics.hpp"
#include "check.hpp"

using namespace gputc;

static void test_roundtrip() {
  std::mt19937 rng(1);
  for (int i = 0; i < 100000; ++i) {
    Fields f{uint32_t(rng()), bool(rng() & 1), uint32_t(rng() & MASK_EPOCH),
             uint32_t(rng() & MASK_AVAIL), uint32_t(rng() & MASK_BASE)};
    Fields g = unpack(pack(f));
    CHECK(g.claimed == f.claimed && g.closed == f.closed && g.epoch == f.epoch &&
          g.avail == f.avail && g.base == f.base);
  }
}

// Sequential claims of mixed widths must tile [0, avail) exactly, and every
// ticket past the end must be invalid.
static void test_tiling() {
  std::mt19937 rng(2);
  for (uint32_t avail = 0; avail <= 200; ++avail) {
    for (int trial = 0; trial < 20; ++trial) {
      uint64_t w = pack(Fields{0, false, 3, avail, MASK_BASE - 5});
      std::vector<int> hit(avail, 0);
      for (int k = 0; k < int(avail) + 10; ++k) {
        uint32_t n = 1 + rng() % 9;
        uint64_t old = w;
        w += claim_increment(n);
        Ticket t = interpret(old, n);
        if (!t.valid) { CHECK(unpack(old).claimed >= avail); continue; }
        CHECK(t.epoch == 3 && t.base == MASK_BASE - 5);
        CHECK(t.n >= 1 && t.n <= n && t.first + t.n <= avail);
        for (uint32_t j = 0; j < t.n; ++j) hit[t.first + j]++;
      }
      for (uint32_t c = 0; c < avail; ++c) CHECK(hit[c] == 1);
      Fields f = unpack(w);
      CHECK(!f.closed && f.epoch == 3 && f.avail == avail && f.base == MASK_BASE - 5);
    }
  }
}

// Overclaim can wrap the claim counter without touching the low half, but the
// wrapped counter makes issued chunks claimable again. Only probing before
// claiming keeps an exhausted word from getting there.
static void test_claim_wrap() {
  uint64_t w = pack(Fields{0xFFFFFFF0u, false, 7, 100, 42});
  for (int i = 0; i < 64; ++i) w += claim_increment(1);
  Fields f = unpack(w);
  CHECK(f.epoch == 7 && f.avail == 100 && f.base == 42 && !f.closed);
  CHECK(f.claimed == 48 && stealable(w));
  CHECK(!stealable(pack(Fields{100, false, 0, 100, 0})));
  CHECK(!stealable(closed_word()));
  CHECK(stealable(pack(Fields{99, false, 0, 100, 0})));
  CHECK(!interpret(closed_word(), 1).valid);
}

// Concurrent claims racing one close: valid tickets are exactly the prefix
// counted by taken(pre_close), with no gaps or overlaps.
static void test_close_race() {
  for (int trial = 0; trial < 200; ++trial) {
    const uint32_t avail = 1 + trial % 150;
    uint64_t word = pack(Fields{0, false, 1, avail, 0});
    std::atomic<bool> go{false};
    std::vector<std::vector<Ticket>> got(4);
    std::vector<std::thread> thieves;
    for (int t = 0; t < 4; ++t) {
      thieves.emplace_back([&, t] {
        std::mt19937 rng(trial * 17 + t);
        while (!go.load()) {}
        for (int i = 0; i < 200; ++i) {
          uint32_t n = 1 + rng() % 4;
          if (!stealable(HostScope::load_relaxed(&word))) continue;
          Ticket k = interpret(HostScope::fetch_add_acq_rel(&word, claim_increment(n)), n);
          if (k.valid) got[t].push_back(k);
        }
      });
    }
    go = true;
    std::this_thread::yield();
    uint64_t pre = HostScope::fetch_or_acq_rel(&word, CLOSED_BIT);
    for (auto& th : thieves) th.join();

    std::vector<int> hit(avail, 0);
    uint32_t sum = 0;
    for (auto& v : got)
      for (auto& k : v) {
        sum += k.n;
        for (uint32_t j = 0; j < k.n; ++j) hit[k.first + j]++;
      }
    CHECK(sum == taken(pre));
    for (uint32_t c = 0; c < avail; ++c) CHECK(hit[c] == (c < sum ? 1 : 0));
  }
}

int main() {
  test_roundtrip();
  test_tiling();
  test_claim_wrap();
  test_close_race();
  std::printf("test_steal_word: ok\n");
}
