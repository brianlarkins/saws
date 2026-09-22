// Torture tests for SawsRing under std::thread. Task bodies are plain memory,
// so running under -fsanitize=thread also checks the release/acquire chain:
// owner writes -> publish -> claim -> read -> retire -> reclaim -> overwrite.

#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "saws_ring.hpp"
#include "atomics.hpp"
#include "check.hpp"

using namespace gputc;

struct Body {
  uint32_t id;
  uint32_t pad[3];
};

struct Seen {
  explicit Seen(uint32_t n) : count(new std::atomic<uint8_t>[n]), n(n) {
    for (uint32_t i = 0; i < n; ++i) count[i] = 0;
  }
  void mark(uint32_t id) { CHECK(id < n); count[id].fetch_add(1); }
  void check_all_once() const {
    for (uint32_t i = 0; i < n; ++i) CHECK(count[i].load() == 1);
  }
  std::unique_ptr<std::atomic<uint8_t>[]> count;
  uint32_t n;
};

template<class Ring>
static bool steal_once(Ring& ring, std::mt19937& rng, uint32_t max_n, Seen& seen) {
  if (!stealable(ring.probe())) return false;
  uint32_t n = 1 + rng() % max_n;
  Ticket t = ring.claim(n);
  if (!t.valid) return false;
  for (uint32_t j = 0; j < t.n; ++j) {
    uint32_t p = ring.chunk_pos(t, j);
    for (uint32_t i = 0; i < Ring::chunk; ++i) seen.mark(ring[p + i].id);
  }
  ring.retire(t);
  return true;
}

// One owner pushing, popping, releasing and reacquiring against thieves: the
// shape of a warp deque.
template<class Cfg>
static void test_owner_vs_thieves(uint32_t cap_chunks, int nthieves, uint32_t ntasks, unsigned seed) {
  using Ring = SawsRing<Body, HostScope, Cfg>;
  constexpr uint32_t C = Cfg::chunk;
  RingState<Cfg::epochs> state;
  state.init();
  std::vector<Body> storage(cap_chunks * C);
  Ring ring(&state, storage.data(), cap_chunks);
  Seen seen(ntasks);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> stolen{0};

  std::vector<std::thread> thieves;
  for (int t = 0; t < nthieves; ++t)
    thieves.emplace_back([&, t] {
      std::mt19937 rng(seed * 31 + t);
      while (!done.load(std::memory_order_relaxed))
        if (steal_once(ring, rng, 3, seen)) stolen++;
        else std::this_thread::yield();
    });

  std::mt19937 rng(seed);
  uint32_t next = 0;
  auto pop_consume = [&] {
    Range r = ring.pop(C);
    if (r.n == 0 && ring.reacquire() > 0) r = ring.pop(C);
    for (uint32_t i = 0; i < r.n; ++i) seen.mark(ring[r.pos + i].id);
    return r.n;
  };

  while (next < ntasks) {
    uint32_t op = rng() % 10;
    if (op < 5) {
      uint32_t n = min_u32(1 + rng() % C, ntasks - next);
      uint32_t pos;
      if (ring.try_push_reserve(n, pos)) {
        for (uint32_t i = 0; i < n; ++i) ring[pos + i].id = next++;
      } else if (pop_consume() == 0) {
        std::this_thread::yield();  // full of chunks thieves are still copying
      }
    } else if (op < 8) {
      pop_consume();
    } else if (op < 9) {
      uint32_t full = ring.private_size() / C;
      if (full) ring.release(full > 1 ? full / 2 : 1);
    } else {
      ring.reclaim();
    }
  }
  while (pop_consume() > 0) {}

  done = true;
  for (auto& th : thieves) th.join();
  ring.reclaim();
  CHECK(ring.used() == 0);
  CHECK(ring.private_size() == 0 && ring.public_chunks() == 0);
  seen.check_all_once();
  std::printf("  owner-vs-thieves cap=%u chunks, %d thieves: %llu of %u tasks stolen\n",
              cap_chunks, nthieves, (unsigned long long)stolen.load(), ntasks);
}

// Several producers promoting into one ring under a lock, thieves claiming
// mixed widths: the shape of the block's global ring.
template<class Cfg>
static void test_promoters_vs_thieves(uint32_t cap_chunks, int nproducers, int nthieves,
                                      uint32_t chunks_each, unsigned seed) {
  using Ring = SawsRing<Body, HostScope, Cfg>;
  constexpr uint32_t C = Cfg::chunk;
  RingState<Cfg::epochs> state;
  state.init();
  std::vector<Body> storage(cap_chunks * C);
  Ring ring(&state, storage.data(), cap_chunks);
  const uint32_t ntasks = nproducers * chunks_each * C;
  Seen seen(ntasks);
  std::mutex lock;
  std::atomic<int> producing{nproducers};

  std::vector<std::thread> threads;
  for (int t = 0; t < nthieves; ++t)
    threads.emplace_back([&, t] {
      std::mt19937 rng(seed * 7 + t);
      for (;;) {
        bool last = producing.load() == 0;
        if (!steal_once(ring, rng, 5, seen)) {
          if (last && !stealable(ring.probe())) return;
          std::this_thread::yield();
        }
      }
    });
  for (int p = 0; p < nproducers; ++p)
    threads.emplace_back([&, p] {
      std::mt19937 rng(seed * 13 + p);
      uint32_t id = p * chunks_each * C, end = id + chunks_each * C;
      while (id < end) {
        uint32_t k = min_u32(1 + rng() % 3, (end - id) / C);
        std::unique_lock<std::mutex> g(lock);
        uint32_t pos;
        if (!ring.try_push_reserve(k * C, pos)) {
          g.unlock();
          std::this_thread::yield();
          continue;
        }
        for (uint32_t i = 0; i < k * C; ++i) ring[pos + i].id = id++;
        while (!ring.can_release()) std::this_thread::yield();
        CHECK(ring.release(k));
      }
      producing--;
    });
  for (auto& th : threads) th.join();

  CHECK(ring.reacquire() == 0);
  ring.reclaim();
  CHECK(ring.used() == 0);
  seen.check_all_once();
  std::printf("  promoters-vs-thieves cap=%u chunks, %d producers, %d thieves: %u tasks\n",
              cap_chunks, nproducers, nthieves, ntasks);
}

struct TinyCfg { static constexpr int chunk = 4; static constexpr int epochs = 4; };
struct WideCfg { static constexpr int chunk = 8; static constexpr int epochs = 16; };

int main(int argc, char** argv) {
  uint32_t scale = argc > 1 ? uint32_t(std::atoi(argv[1])) : 1;
  std::printf("test_saws_ring:\n");
  for (unsigned seed = 1; seed <= 3; ++seed) {
    test_owner_vs_thieves<TinyCfg>(4, 3, 50000 * scale, seed);
    test_owner_vs_thieves<TinyCfg>(16, 7, 50000 * scale, seed);
    test_owner_vs_thieves<WideCfg>(8, 4, 50000 * scale, seed);
    test_promoters_vs_thieves<TinyCfg>(8, 3, 4, 2000 * scale, seed);
    test_promoters_vs_thieves<WideCfg>(64, 4, 6, 2000 * scale, seed);
  }
  std::printf("test_saws_ring: ok\n");
}
