#pragma once

// UTS tree generation as __host__ __device__ code: the SHA-1 splittable RNG
// of rng/brg_sha1.c and the child rules of uts.c, with the tree parameters
// passed explicitly instead of read from globals.

#include <cmath>
#include <cstdint>

#include "common.hpp"

namespace uts { constexpr int GEO_DEPTHS = 256; }

struct UtsParams {
  int    type;           // tree_t
  double b_0;
  int    gen_mx;
  int    shape_fn;       // geoshape_t
  int    nonLeafBF;
  double nonLeafProb;
  double shiftDepth;
  int    computeGranularity;
  double geo_log_q[uts::GEO_DEPTHS];  // uts::geo_log_q(depth), filled on the host
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

GPUTC_HD uint32_t get_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

GPUTC_HD void put_be32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// SHA-1 of one padded 64-byte block, as big-endian words. The rounds are fully
// unrolled so every index into w is a constant and w stays in registers.
GPUTC_HD void sha1_block(uint32_t w[16], uint8_t out[20]) {
  uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u, e = 0xc3d2e1f0u;
  GPUTC_UNROLL
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
  put_be32(out + 0,  0x67452301u + a);
  put_be32(out + 4,  0xefcdab89u + b);
  put_be32(out + 8,  0x98badcfeu + c);
  put_be32(out + 12, 0x10325476u + d);
  put_be32(out + 16, 0xc3d2e1f0u + e);
}

// SHA-1 of the 20-byte message {16 zero bytes, seed}.
GPUTC_HD void rng_init(uint8_t state[20], int seed) {
  uint32_t w[16] = {0, 0, 0, 0, uint32_t(seed), 0x80000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20 * 8};
  sha1_block(w, state);
}

// SHA-1 of the 24-byte message {parent state, spawn_number}.
GPUTC_HD void rng_spawn(const uint8_t parent[20], uint8_t child[20], int spawn_number) {
  uint32_t w[16] = {get_be32(parent), get_be32(parent + 4), get_be32(parent + 8), get_be32(parent + 12),
                    get_be32(parent + 16), uint32_t(spawn_number), 0x80000000u,
                    0, 0, 0, 0, 0, 0, 0, 0, 24 * 8};
  sha1_block(w, child);
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

// log(1 - p) for the geometric child distribution of a node at this depth,
// whose expected branching factor b_i comes from the shape function.
GPUTC_HD double geo_log_q(int depth, const UtsParams& p) {
  double b_i = p.b_0;
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
  return ::log(1 - prob);
}

// Depths below GEO_DEPTHS read log(1 - p) from the table the host filled, which
// also keeps FP64 transcendentals off the GPU's slow double-precision units.
GPUTC_HD int num_children_geo(const UtsNode& n, const UtsParams& p) {
  double log_q = n.height < GEO_DEPTHS ? p.geo_log_q[n.height] : geo_log_q(n.height, p);
  double u = to_prob(rng_rand(n.state));
  return int(::floor(::log(1 - u) / log_q));
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
