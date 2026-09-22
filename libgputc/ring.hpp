#pragma once

// Non-owning view over power-of-two ring storage. Positions are free-running
// uint32 counters; the capacity must divide 2^32 so they may wrap freely.

#include "common.hpp"

namespace gputc {

struct Span {
  uint32_t index;  // physical index of the first element
  uint32_t len;
};

// A logical range split at the physical end of the ring.
struct Spans {
  Span first;
  Span second;     // len == 0 unless the range wraps
};

template<class T>
struct RingView {
  T*       data;
  uint32_t capacity;

  GPUTC_HD uint32_t index(uint32_t pos) const { return pos & (capacity - 1); }
  GPUTC_HD T& operator[](uint32_t pos) const { return data[index(pos)]; }

  GPUTC_HD Spans spans(uint32_t pos, uint32_t n) const {
    uint32_t i = index(pos);
    uint32_t first = min_u32(n, capacity - i);
    return Spans{Span{i, first}, Span{0, n - first}};
  }
};

} // namespace gputc
