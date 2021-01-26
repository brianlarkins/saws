#include <iostream> 
#include <cassert> 
#include <cstdlib> 
 
extern "C" {
#include "uts.h"
}

#include "UTSIterator.h"

// Root Iterator
UTSIterator::UTSIterator(int type) {
  node.type   = type;
  node.height = 0;
  rng_init(node.state.state, rootId);

  node.numChildren = uts_numChildren(&node);
  current_child    = 0;
  processed        = 0;
}

// NULL Iterator
UTSIterator::UTSIterator() {
  node.numChildren = 0;
  processed        = 1;
}

// Child Iterator
UTSIterator::UTSIterator(Node _node) {
  node          = _node;
  current_child = 0;
  processed     = 0;
}

bool UTSIterator::hasNext() {
  return current_child < node.numChildren;
}

UTSIterator UTSIterator::next() {
  Node child;

  assert(hasNext());

  child.type   = uts_childType(&node);
  child.height = node.height + 1;

  for (int j = 0; j < computeGranularity; j++) {
    rng_spawn(node.state.state, child.state.state, current_child);
  }

  child.numChildren = uts_numChildren(&child);
  ++current_child;

  return UTSIterator(child); // TODO: This is not the most efficient way to do this, lots of copying...
}


void UTSIterator::next(UTSIterator *nextit) {
  assert(hasNext());

  nextit->node.type   = uts_childType(&node);
  nextit->node.height = node.height + 1;

  for (int j = 0; j < computeGranularity; j++) {
    rng_spawn(node.state.state, nextit->node.state.state, current_child);
  }

  nextit->node.numChildren  = uts_numChildren(&nextit->node);
  nextit->processed         = 0;
  nextit->current_child     = 0;

  ++current_child;
}


void UTSIterator::process() {
  if (!processed) {
    nNodes++;
    if (node.numChildren == 0) nLeaves++;
    if ((counter_t) node.height > maxDepth) maxDepth = node.height;
  }
}


void UTSIterator::resetStats() {
  nNodes   = 0;
  nLeaves  = 0;
  maxDepth = 0;
}

counter_t UTSIterator::get_nNodes() {
  return nNodes;
}

counter_t UTSIterator::get_nLeaves() {
  return nLeaves;
}

counter_t UTSIterator::get_maxDepth() {
  return maxDepth;
}

counter_t UTSIterator::nNodes   = 0;
counter_t UTSIterator::nLeaves  = 0;
counter_t UTSIterator::maxDepth = 0;

