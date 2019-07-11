/*
 * Copyright (C) 2018. See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>

#include "tc.h"

/** Register a task collection and return a portable handle.  Collective operation.
  * @param  tc Pointer to the task collection.
  * @return    Portable reference
  */
gtc_t gtc_handle_register(tc_t *tc) {
  int   i;
  gtc_t gtc;

  // Find the first free slot in the GTC directory
  gtc = -1;
  for (i = 0; i < GTC_MAX_TC; i++) {
    if (_c->tcs[i] == NULL) {
      _c->tcs[i] = tc;
      gtc = i;
      _c->total_tcs++;
      break;
    }
  }

  // Make sure we found a free slot in the directory
  assert(gtc >= 0);

  return gtc;
}


/** Free a GTC handle.  Collective operation.
  * @param  gtc Reference to free
  * @return     Local pointer to the TC whose handle was freed
  */
tc_t * gtc_handle_release(gtc_t gtc) {
  tc_t *tc = _c->tcs[gtc];

  // Update the directory
  _c->tcs[gtc] = NULL;
  _c->total_tcs--;

  return tc;
}
