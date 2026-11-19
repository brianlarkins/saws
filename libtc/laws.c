/********************************************************/
/*                                                      */
/*  laws.c - node work bitmap for locality-aware work   */
/*           stealing (LAWS, E2)                        */
/*                                                      */
/********************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tc.h"

/*
 * Each node has one 64-bit work bitmap: bit i is set while node-mate i (its index
 * in the node, _c->loc.node_rank_of) has stealable work.  The bitmap is the node
 * leader's copy of the symmetric word tc->node_bits.
 *
 *  - Owner-only writes: each PE sets and clears only its own bit, with CPU atomics
 *    through shmem_ptr, and only when its state changes.  Thieves never write bits.
 *    The bit is set at release (an event) and cleared lazily: every laws_poll progress
 *    calls the owner checks whether thieves have claimed everything.
 *  - Node-mates read the bitmap with plain loads (shmem_ptr).
 *  - Thieves on other nodes read it with an 8-byte shmem get, a read rather than an
 *    atomic, so the word stays in the CPU atomicity domain ("one word, one domain").
 * Bits are hints: a set bit may stay set for up to laws_poll of the owner's progress
 * calls after thieves drain it; a thief that picks it pays only a peek miss (a load).
 */



/** collective: allocate the bitmap word and find my node leader's copy */
void gtc_laws_create(tc_t *tc) {
  gtc_locality_t *L = &_c->loc;

  // its own cache line, so bitmap traffic does not falsely share with other data
  tc->node_bits = shmem_align(64, 64);
  assert(tc->node_bits);
  *tc->node_bits = 0;
  tc->bit_set = 0;
  tc->my_node_bits = NULL;
  {
    unsigned p = 1;
    while (p < (unsigned)tc->ldbal_cfg.laws_poll)
      p <<= 1;
    tc->laws_poll_mask = p - 1;
  }
  if (L->ptr_ok && L->node_size <= 64)
    tc->my_node_bits = shmem_ptr(tc->node_bits, gtc_node_leader(L->node));
  shmem_barrier_all();
}



/** collective */
void gtc_laws_destroy(tc_t *tc) {
  shmem_barrier_all();
  shmem_free(tc->node_bits);
  tc->node_bits = NULL;
  tc->my_node_bits = NULL;
}



/** owner only: publish whether I have stealable work (writes only on change) */
void gtc_laws_set_bit(tc_t *tc, int have_work) {
  uint64_t mask;

  have_work = (have_work != 0);
  if (have_work == tc->bit_set || !tc->my_node_bits)
    return;
  mask = 1ULL << _c->loc.node_rank;
  if (have_work)
    __atomic_fetch_or(tc->my_node_bits, mask, __ATOMIC_RELEASE);
  else
    __atomic_fetch_and(tc->my_node_bits, ~mask, __ATOMIC_RELEASE);
  tc->bit_set = have_work;
  tc->ct.laws_bit_writes++;
}



/** a uniformly random set bit of a nonzero word */
static int random_set_bit(uint64_t bits) {
  int k = rand() % __builtin_popcountll(bits);

  while (k-- > 0)
    bits &= bits - 1;          // clear the lowest set bit
  return __builtin_ctzll(bits);
}



/** owner, laws_eager: clear my bit now that I know nothing of mine is public */
void gtc_laws_eager_clear(tc_t *tc) {
  if (tc->ldbal_cfg.laws_eager && tc->bit_set) {
    gtc_laws_set_bit(tc, 0);
    tc->ct.laws_eager_clears++;
  }
}



/**
 * TARGET_LAWS victim selection:
 *   1. a random node-mate whose bit is set (skipping node-mates found empty during
 *      this search);
 *   2. otherwise, if far_dir: up to far_dir_tries random other nodes, reading each
 *      node's bitmap remotely, and a random PE whose bit is set there;
 *   3. otherwise a random off-node PE (a blind probe).
 * With laws_near_pct < 100, a step skips 1 with probability (100 - laws_near_pct)%,
 * so some probes leave the node even while node-mates have work (E2c: strict
 * local-first recirculates work inside a node on binomial UTS, as sweep did in E1).
 */
int gtc_laws_select(tc_t *tc, gtc_vs_state_t *state) {
  gtc_locality_t *L = &_c->loc;
  uint64_t bits;

  if (L->far_count > 0 && tc->ldbal_cfg.laws_near_pct < 100
      && rand() % 100 >= tc->ldbal_cfg.laws_near_pct) {
    tc->ct.laws_near_skip++;
    goto far;
  }

  // 1. my node
  bits = __atomic_load_n(tc->my_node_bits, __ATOMIC_ACQUIRE);
  tc->ct.laws_bitmap_reads++;
  tc->ct.laws_occ[(__builtin_popcountll(bits) * 10) / L->node_size]++;
  bits &= ~(1ULL << L->node_rank);
  bits &= ~state->near_miss;
  if (bits)
    return L->node_pes[random_set_bit(bits)];
  tc->ct.laws_bitmap_empty++;

  // single node: nothing off-node; keep probing node-mates so termination proceeds
  if (L->far_count == 0) {
    state->near_miss = 0;
    return gtc_random_near_peer();
  }

far:
  // 2. other nodes' bitmaps (E2b)
  if (tc->ldbal_cfg.far_dir) {
    for (int t = 0; t < tc->ldbal_cfg.far_dir_tries; t++) {
      int n = gtc_random_other_node();
      uint64_t w = shmem_uint64_g(tc->node_bits, gtc_node_leader(n));
      tc->ct.laws_fardir_reads++;
      if (w)
        return gtc_node_member(n, random_set_bit(w));
      tc->ct.laws_fardir_empty++;
    }
  }

  // 3. blind off-node probe
  tc->ct.laws_blind_far++;
  return gtc_random_far_peer();
}
