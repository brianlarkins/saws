#include <stdio.h>
#include <stdlib.h>
#include "clod.h"

/*
 * Common Local Object Database Implementation
 */

/** Create a new CLOD.  This is a collective call.
 *  \param max_size Max number of entries in this CLOD.
 */
clod_t clod_create(int max_size) {
  clod_t clod = malloc(sizeof(struct clod_s) + max_size*sizeof(void*));

  clod->max_size = max_size;
  clod->nextfree = 0;

  return clod;
}

/** Destroy and deallocate an existing CLOD.  This is a collective call.
 *  \param clod The clod to be destroyed.
 */
void clod_destroy(clod_t clod) {
  free(clod);
}

/** Reset a CLOD so it can be reused.  This is a collective call.
 *  \param clod The clod to be reset.
 */
void clod_reset(clod_t clod) {
  clod->nextfree = 0;
}

/** Look up an entry in the CLOD.
 *  \param clod The clod to perform the lookup on.
 *  \param id   The id to look up.
 *  \return     A pointer to the CLO.
 */
void *clod_lookup(clod_t clod, clod_key_t id) {
  // Make sure this id is valid on this clod
  assert(id >= 0 && id < clod->nextfree);

  return clod->objects[id];
}

/** Update an entry in the CLOD with a new pointer.  This is a collective call.
 *  \param clod   The clod that contains the object to be updated.
 *  \param id     The id of the object to assign to.
 *  \param target Local pointer to the new object to reference in <clod, id>.
 */
void clod_assign(clod_t clod, clod_key_t id, void *target) {
  // Make sure this id is valid on this clod
  assert(id >= 0 && id < clod->nextfree);

  clod->objects[id] = target;
}

/** Allocate a new entry in the CLOD.  Once entries are allocated they cannot be
 *  freed, space in the CLOD can only be reclaimed by calling clod_reset().
 *  This is a collective call.
 *  \param clod The clod to allocate on.
 */
clod_key_t clod_nextfree(clod_t clod) {
  int id;

  // Is there still space available in this clod?
  assert(clod->nextfree < clod->max_size);

  id = clod->nextfree;
  clod->nextfree++;
  return id;
}

