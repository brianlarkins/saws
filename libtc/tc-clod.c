#include <stdio.h>
#include "tc.h"
#include "clod.h"

/** Create a Common Local Object association, clod_key_t -> void *ptr.
 *  This is a collective call.
 *
 *  \param tc  Task collection
 *  \param ptr Pointer to the Common Local Object
 *  \return    Portable reference that can be used to look up ptr
 */
clod_key_t gtc_clo_associate(gtc_t gtc, void *ptr) {
  tc_t *tc = gtc_lookup(gtc);

  clod_key_t id = clod_nextfree(tc->clod);
  clod_assign(tc->clod, id, ptr);
  return id;
}

/** Lookup a Common Local Object, using its portable ID.
 *
 *  \param tc Task collection
 *  \param id Portable reference to a CLO.
 *  \return   Pointer to the CLO.
 */
void *gtc_clo_lookup(gtc_t gtc, clod_key_t id) {
  tc_t *tc = gtc_lookup(gtc);

  return clod_lookup(tc->clod, id);
}

/** Clear out all Common Local Object associations.  CLO associations
 *  are not reset when a TC is reset.  This is a collective call.
 *
 *  \param tc Task collection
 */
void gtc_clo_reset(gtc_t gtc) {
  tc_t *tc = gtc_lookup(gtc);

  clod_reset(tc->clod);
}
