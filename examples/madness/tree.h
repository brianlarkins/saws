#ifndef _treedef_h
#define _treedef_h

#include <unistd.h>
#include <sys/types.h>

#include "tensor.h"

#define NUM_CHILDREN 8

enum coeff_e { madCoeffNone, madCoeffScaling, madCoeffWavelet, madCoeffBoth };
typedef enum coeff_e coeff_t;


struct treeptr_s {
  int proc;
  int index;
};
typedef struct treeptr_s treeptr_t;


struct treedata_s {
  long level;
  long x;
  long y;
  long z;
  coeff_t valid;
  tensor3dk_t  s;  // scaling (sum) coefficients
  tensor3d2k_t d;  // wavelet (difference) coefficients
};
typedef struct treedata_s treedata_t;


struct tree_s {
  u_int8_t       flags;
  treeptr_t      parent;
  //treeptr_t    children[NUM_CHILDREN];
  struct tree_s *children;
  treedata_t     data;
};
typedef struct tree_s tree_t;


struct mad_tree_s {
  void    **nodes;
  int       me, nproc;
  int       max_nodes;
  int       next_free;
  tree_t   *root;
};
typedef struct mad_tree_s mad_tree_t;
  

// tree.c 
mad_tree_t  *create_tree();
tree_t      *get_root(mad_tree_t *ftree);
tree_t      *get_child(mad_tree_t *ftree, tree_t *node, int childidx);
long         get_level(mad_tree_t *ftree, tree_t *node);
int          has_scaling(mad_tree_t *ftree, tree_t *node);
tensor_t    *get_scaling(mad_tree_t *f, tree_t *node);
void         set_scaling(mad_tree_t *f, tree_t *node, tensor_t *scoeffs);
void         get_xyzindex(mad_tree_t *ftree, tree_t *node, long *x, long *y, long *z);

tree_t      *node_alloc(long level, long x, long y, long z);
void         node_free(tree_t *node);

tree_t      *set_child(mad_tree_t *ftree, tree_t *parent, long level, long x, long y, long z, int childidx);
void         set_children(mad_tree_t *ftree, tree_t *node);

#if 0
//void           set_root(mad_tree_t ftree, treeptr_t *root);
treeptr_t      *set_child(mad_tree_t ftree, treeptr_t *parent, long level, long x, long y, long z, int childidx);
void           free_node(mad_tree_t ftree, treeptr_t *node);
void           set_level(mad_tree_t ftree, treeptr_t *node, long level);
void           set_xyzindex(mad_tree_t ftree, treeptr_t *node, long x, long y, long z);
void           set_xindex(mad_tree_t ftree, treeptr_t *node, long x);
void           set_yindex(mad_tree_t ftree, treeptr_t *node, long y);
void           set_zindex(mad_tree_t ftree, treeptr_t *node, long z);
void           set_scaling(func_t *f, treeptr_t *node, tensor_t *scoeffs);
void           set_wavelet(func_t *f, treeptr_t *node, tensor_t *dcoeffs);
#define        set_left( t,p,l,i)	set_child(t, p, l, i, MAD_CHILD_LEFT)
#define        set_right(t,p,l,i)	set_child(t, p, l, i, MAD_CHILD_RIGHT)
void	       print_node(mad_tree_t ftree, treeptr_t *t);
#endif

#endif // _treedef_h

