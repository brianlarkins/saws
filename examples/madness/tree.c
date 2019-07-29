#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tc.h>

#include "tree.h"
#include "tensor.h"


/*
 * create_tree - collective call to create function tree
 */
#if 0
mad_tree_t *create_tree(int max_nodes) {
  mad_tree_t *t = malloc(sizeof(mad_tree_t));
  assert(t != NULL);

  MPI_Comm_rank(MPI_COMM_WORLD, &t->me);
  MPI_Comm_size(MPI_COMM_WORLD, &t->nproc);
  
  t->max_nodes  = max_nodes;
  t->next_free  = 0;

  t->nodes = malloc(sizeof(void*)*t->nproc);

  ARMCI_Malloc(t->nodes, max_nodes*sizeof(tree_t));

  return t;
}
#endif

mad_tree_t *create_tree() {
  mad_tree_t *t = calloc(1, sizeof(mad_tree_t));
  assert(t != NULL);

  t->me = _c->rank;
  t->nproc = _c->size;

  // FIXME: Root is only valid on process 0!
  t->root = node_alloc(0, 0, 0, 0);
  
  return t;
}


tree_t *node_alloc(long level, long x, long y, long z) {
  tree_t *n = calloc(1, sizeof(tree_t));

  n->data.level = level;
  n->data.x = x;
  n->data.y = y;
  n->data.z = z;
  n->data.valid = madCoeffNone;
  n->children   = NULL;

  return n;
}


void node_free(tree_t *node) {
  if (node->children) free(node->children);
  free(node);
}


tree_t *get_root(mad_tree_t *ftree) {
  return ftree->root;
}
  

tree_t *get_child(mad_tree_t *ftree, tree_t *node, int childidx) {
  return &node->children[childidx];
}


long get_level(mad_tree_t *ftree, tree_t *node) {
  return node->data.level;
}


void get_xyzindex(mad_tree_t *ftree, tree_t *node, long *x, long *y, long *z) {
  *x = node->data.x;
  *y = node->data.y;
  *z = node->data.z;
}


long get_xindex(mad_tree_t *ftree, tree_t *node) {
  return node->data.x;
}


long get_yindex(mad_tree_t *ftree, tree_t *node) {
  return node->data.y;
}


long get_zindex(mad_tree_t *ftree, tree_t *node) {
  return node->data.z;
}


int has_scaling(mad_tree_t *ftree, tree_t *node) { 
  return ((node->data.valid == madCoeffScaling) || (node->data.valid == madCoeffBoth)); 
} 


tensor_t *get_scaling(mad_tree_t *f, tree_t *node) {
  assert(node);
  //printf(" %d: scaling\n", context->mythread);
  if ((node->data.valid == madCoeffScaling) || (node->data.valid == madCoeffBoth)) {
    return tensor_copy((tensor_t *)&node->data.s);
  } else
    return NULL;
}


tree_t *set_child(mad_tree_t *ftree, tree_t *parent, long level, long x, long y, long z, int childidx) {
  tree_t *cnode = get_child(ftree, parent, childidx);
  
  cnode->data.level = level;
  cnode->data.x = x;
  cnode->data.y = y;
  cnode->data.z = z;
  cnode->data.valid = madCoeffNone;
  cnode->children   = NULL;

  return cnode;
}


void set_level(mad_tree_t *ftree, tree_t *node, long level) {
  node->data.level = level;
}


void set_xyzindex(mad_tree_t *ftree, tree_t *node, long x, long y, long z) {
  node->data.x = x;
  node->data.y = y;
  node->data.z = z;
}


void set_scaling(mad_tree_t *f, tree_t *node, tensor_t *scoeffs) {
  assert(node);

  if (!scoeffs) {
    node->data.valid = (node->data.valid == madCoeffBoth) ? madCoeffWavelet : madCoeffNone;

  } else {
    memcpy(&node->data.s, scoeffs, sizeof(tensor3dk_t));

    // technically need to check for both && wavelet
    node->data.valid = (node->data.valid == madCoeffWavelet) ? madCoeffBoth : madCoeffScaling;
  }
}


void set_children(mad_tree_t *ftree, tree_t *node) {
  long level = get_level(ftree, node);
  long i,j,k;
  long x,y,z;

  if (node->children == NULL) {
    node->children = calloc(8, sizeof(tree_t));
    assert(node->children != NULL);
  }
  
  x = 2*get_xindex(ftree, node);
  y = 2*get_yindex(ftree, node);
  z = 2*get_zindex(ftree, node);

  for (i = 0; i < 2; i++)
    for (j = 0; j < 2; j++)
      for (k = 0; k < 2; k++)
        set_child(ftree, node, level+1, x+i, y+j, z+k, i*4+j*2+k);

}

