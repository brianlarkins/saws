#ifndef __CLOD_H__
#define __CLOD_H__

#include <assert.h>
#include <inttypes.h>

/** Define the Common Local Objects interface **/

// A Common Local Object Database (CLOD)
struct clod_s {
  int   max_size;
  int   nextfree;
  void *objects[0];
};

typedef struct clod_s* clod_t;
typedef int64_t        clod_key_t;

clod_t clod_create(int max_size);
void   clod_destroy(clod_t clod);
void   clod_reset(clod_t clod);

void  *clod_lookup(clod_t clod, clod_key_t id);
void   clod_assign(clod_t clod, clod_key_t id, void *target);
clod_key_t clod_nextfree(clod_t clod);

#endif /* __CLOD_H__ */
