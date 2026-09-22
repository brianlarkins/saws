#pragma once

// UTS tree generation as __host__ __device__ code: the SHA-1 splittable RNG
// of rng/brg_sha1.c and the child rules of uts.c, with the tree parameters
// passed explicitly instead of read from globals.

#include <cmath>
#include <cstdint>

#include "common.hpp"

struct UtsParams {
  int    type;           // tree_t
  double b_0;
  int    gen_mx;
  int    shape_fn;       // geoshape_t
  int    nonLeafBF;
  double nonLeafProb;
  double shiftDepth;
  int    computeGranularity;
};

struct UtsNode {
  int     type;
  int     height;
  int     numChildren;
  uint8_t state[20];
};

namespace uts {

enum { BIN = 0, GEO, HYBRID, BALANCED };
enum { LINEAR = 0, EXPDEC, CYCLIC, FIXED };
constexpr int MAXNUMCHILDREN = 100;

GPUTC_HD uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// SHA-1 of a message of at most 55 bytes (one block).
GPUTC_HD void sha1(const uint8_t* msg, int len, uint8_t out[20]) {
  uint32_t w[16];
  for (int i = 0; i < 16; ++i) w[i] = 0;
  for (int i = 0; i < len; ++i) w[i >> 2] |= uint32_t(msg[i]) << (24 - 8 * (i & 3));
  w[len >> 2] |= 0x80u << (24 - 8 * (len & 3));
  w[15] = uint32_t(len) * 8;

  uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int t = 0; t < 80; ++t) {
    uint32_t wt;
    if (t < 16) {
      wt = w[t];
    } else {
      wt = rotl(w[(t + 13) & 15] ^ w[(t + 8) & 15] ^ w[(t + 2) & 15] ^ w[t & 15], 1);
      w[t & 15] = wt;
    }
    uint32_t f, k;
    if (t < 20)      { f = d ^ (b & (c ^ d));       k = 0x5a827999u; }
    else if (t < 40) { f = b ^ c ^ d;               k = 0x6ed9eba1u; }
    else if (t < 60) { f = (b & c) | (d & (b ^ c)); k = 0x8f1bbcdcu; }
    else             { f = b ^ c ^ d;               k = 0xca62c1d6u; }
    uint32_t tmp = rotl(a, 5) + f + e + k + wt;
    e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  for (int i = 0; i < 20; ++i) out[i] = uint8_t(h[i >> 2] >> (24 - 8 * (i & 3)));
}

GPUTC_HD void put_be32(uint8_t* p, int v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

GPUTC_HD void rng_init(uint8_t state[20], int seed) {
  uint8_t msg[20] = {};
  put_be32(msg + 16, seed);
  sha1(msg, 20, state);
}

GPUTC_HD void rng_spawn(const uint8_t parent[20], uint8_t child[20], int spawn_number) {
  uint8_t msg[24];
  for (int i = 0; i < 20; ++i) msg[i] = parent[i];
  put_be32(msg + 20, spawn_number);
  sha1(msg, 24, child);
}

GPUTC_HD int rng_rand(const uint8_t state[20]) {
  uint32_t b = (uint32_t(state[16]) << 24) | (uint32_t(state[17]) << 16)
             | (uint32_t(state[18]) << 8) | uint32_t(state[19]);
  return int(b & 0x7fffffffu);
}

GPUTC_HD double to_prob(int n) { return n < 0 ? 0.0 : double(n) / 2147483648.0; }

GPUTC_HD int num_children_bin(const UtsNode& n, const UtsParams& p) {
  return to_prob(rng_rand(n.state)) < p.nonLeafProb ? p.nonLeafBF : 0;
}

GPUTC_HD int num_children_geo(const UtsNode& n, const UtsParams& p) {
  double b_i = p.b_0;
  int depth = n.height;
  if (depth > 0) {
    switch (p.shape_fn) {
      case EXPDEC:
        b_i = p.b_0 * ::pow(double(depth), -::log(p.b_0) / ::log(double(p.gen_mx)));
        break;
      case CYCLIC:
        if (depth > 5 * p.gen_mx) { b_i = 0.0; break; }
        b_i = ::pow(p.b_0, ::sin(2.0 * 3.141592653589793 * double(depth) / double(p.gen_mx)));
        break;
      case FIXED:
        b_i = depth < p.gen_mx ? p.b_0 : 0;
        break;
      case LINEAR:
      default:
        b_i = p.b_0 * (1.0 - double(depth) / double(p.gen_mx));
        break;
    }
  }
  double prob = 1.0 / (1.0 + b_i);
  double u = to_prob(rng_rand(n.state));
  return int(::floor(::log(1 - u) / ::log(1 - prob)));
}

GPUTC_HD int num_children(const UtsNode& n, const UtsParams& p) {
  int c = 0;
  switch (p.type) {
    case BIN:      c = n.height == 0 ? int(::floor(p.b_0)) : num_children_bin(n, p); break;
    case GEO:      c = num_children_geo(n, p); break;
    case HYBRID:   c = n.height < p.shiftDepth * p.gen_mx ? num_children_geo(n, p) : num_children_bin(n, p); break;
    case BALANCED: c = n.height < p.gen_mx ? int(p.b_0) : 0; break;
  }
  if (n.height == 0 && n.type == BIN) {
    int root_bf = int(::ceil(p.b_0));
    if (c > root_bf) c = root_bf;
  } else if (c > MAXNUMCHILDREN) {
    c = MAXNUMCHILDREN;
  }
  return c;
}

GPUTC_HD int child_type(const UtsNode& n, const UtsParams& p) {
  if (p.type == HYBRID) return n.height < p.shiftDepth * p.gen_mx ? GEO : BIN;
  return p.type;
}

GPUTC_HD UtsNode make_child(const UtsNode& parent, int i, const UtsParams& p) {
  UtsNode c;
  c.type = child_type(parent, p);
  c.height = parent.height + 1;
  for (int j = 0; j < p.computeGranularity; ++j) rng_spawn(parent.state, c.state, i);
  c.numChildren = num_children(c, p);
  return c;
}

} // namespace uts
