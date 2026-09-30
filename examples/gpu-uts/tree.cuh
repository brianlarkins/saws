#pragma once

#include "rng.cuh"
#include <libgputc/saws.cuh>
#include <cmath>

namespace uts {

enum class Tree : unsigned { binomial, geometric, hybrid, balanced };
enum class Shape : unsigned { linear, exponential, cyclic, fixed };

struct Parameters {
    Tree tree = Tree::geometric;
    Shape shape = Shape::linear;
    double branching = 4.0;
    unsigned depth = 6;
    double nonleaf_probability = 15.0 / 64.0;
    unsigned nonleaf_children = 4;
    double shift_depth = 0.5;
    unsigned granularity = 1;
};

struct Node {
    rng::State random;
    unsigned depth;
};

struct Statistics {
    unsigned long long leaves;
    unsigned depth;
};

__device__ inline unsigned geometric_children(const Node& node, const Parameters& p) {
    double mean = p.branching;
    if (node.depth) {
        switch (p.shape) {
        case Shape::linear:
            mean *= 1.0 - double(node.depth) / p.depth;
            break;
        case Shape::exponential:
            if (mean > 0)
                mean *= ::pow(double(node.depth), -::log(p.branching) / ::log(double(p.depth)));
            break;
        case Shape::cyclic:
            mean = node.depth > 5ull * p.depth ? 0.0 :
                ::pow(p.branching, ::sin(2.0 * 3.141592653589793 * node.depth / p.depth));
            break;
        case Shape::fixed:
            if (node.depth >= p.depth) mean = 0.0;
            break;
        }
    }
    if (mean <= 0.0) return 0;
    const double probability = 1.0 / (1.0 + mean);
    const double count = ::floor(::log(1.0 - rng::probability(node.random)) /
                                 ::log(1.0 - probability));
    return count >= 100.0 ? 100 : unsigned(count);
}

__device__ inline unsigned children(const Node& node, const Parameters& p) {
    unsigned count = 0;
    switch (p.tree) {
    case Tree::binomial:
        if (node.depth == 0) return unsigned(p.branching); // BIN root is uncapped.
        count = rng::probability(node.random) < p.nonleaf_probability ? p.nonleaf_children : 0;
        break;
    case Tree::geometric:
        return geometric_children(node, p);
    case Tree::hybrid:
        if (node.depth < p.shift_depth * p.depth) return geometric_children(node, p);
        count = rng::probability(node.random) < p.nonleaf_probability ? p.nonleaf_children : 0;
        break;
    case Tree::balanced:
        count = node.depth < p.depth ? unsigned(p.branching) : 0;
        break;
    }
    return count > 100 ? 100 : count;
}

struct MakeChild {
    unsigned granularity;

    __device__ Node operator()(const Node& parent, unsigned index) const {
        Node result{parent.random, parent.depth + 1};
        rng::State input = parent.random;
        for (unsigned repeat = 0; repeat < granularity; ++repeat) {
            // A no-op compiler barrier keeps -g repeated hashing from becoming
            // a single common subexpression. The generated tree is unchanged.
            asm volatile("" : "+r"(input.word[0]), "+r"(input.word[1]),
                               "+r"(input.word[2]), "+r"(input.word[3]), "+r"(input.word[4]));
            result.random = rng::child(input, index);
        }
        return result;
    }
};

// This is the application callback. SAWS handles the persistent kernel and
// scheduling; tree generation and application statistics stay here.
struct Execute {
    Parameters params;
    Statistics* statistics;

    __device__ void operator()(saws::Context<Node>& ctx, const Node& node, bool valid) const {
        const unsigned count = valid ? children(node, params) : 0;
        const unsigned leaves = __popc(__ballot_sync(0xffffffffu, valid && count == 0));
        const unsigned depth = ctx.warp_max(valid ? node.depth : 0);
        if (ctx.lane() == 0) {
            atomicAdd(&statistics->leaves, static_cast<unsigned long long>(leaves));
            atomicMax(&statistics->depth, depth);
        }
        if (!ctx.spawn_n(count, node, MakeChild{params.granularity})) return;
    }
};

} // namespace uts
