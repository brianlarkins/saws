#include <vector>

#include "saws_ring.hpp"
#include "atomics.hpp"
#include "check.hpp"

using namespace gputc;

struct Body {
  uint32_t id;
};

template<uint32_t C, uint32_t Cap, int E>
static void test_acquire_sequence() {
  using Ring = SawsRing<Body, HostScope, C, Cap, E>;
  RingState<E> state;
  state.init(0);
  std::vector<Body> storage(Ring::capacity);
  Ring ring(&state, storage.data());

  Range r{};
  CHECK(ring.push_reserve(C, C, &r) == ReserveStatus::Ok && r.n == C);
  CHECK(ring.release_half() == 1);
  CHECK(ring.private_size() == 0);
  CHECK(ring.acquire_half_status() == AcquireStatus::HasPrivate);
  CHECK(ring.private_size() > 0);
  ring.reclaim();
}

int main() {
  test_acquire_sequence<8, 16, 8>();
  std::printf("test_saws_ring: ok\n");
}
