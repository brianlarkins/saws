/********************************************************/
/*                                                      */
/*  reduce.c - SAWS redeuce collective ops              */
/*                                                      */
/*  author: d. brian larkins                            */
/*  created: 3/20/16                                    */
/*                                                      */
/********************************************************/

#include <tc.h>

/**
 * @file
 *
 * wrapper around cumbersome openshmem collectives
 */

static int gtc_collective_size(gtc_datatype_t type);

/**
 * gtc_reduce - blocks process until all processes reach the barrier
 * @param in buffer containing this processes source data
 * @param out buffer containing the final reduction value (only valid on root process)
 * @param op operation to perform
 * @param type type data type of elements
 * @param elems number of elements
 * @returns status of operation
 */
gtc_status_t gtc_reduce(void *in, void *out, gtc_reduceop_t op, gtc_datatype_t type, int elems) {
  void *sin   = shmem_malloc(elems * gtc_collective_size(type));
  void *sout  = shmem_malloc(elems * gtc_collective_size(type));
  void *pWrk  = shmem_malloc(MAX(elems/2+1, SHMEM_REDUCE_MIN_WRKDATA_SIZE) * gtc_collective_size(type));
  static long pSync[SHMEM_REDUCE_SYNC_SIZE];

  for (int i=0; i<SHMEM_REDUCE_SYNC_SIZE; i++)
    pSync[i] = SHMEM_SYNC_VALUE;

  memcpy(sin, in, elems * gtc_collective_size(type));

  switch (op) {
    case GtcReduceOpSum:
      switch(type) {
        case IntType:
          shmem_int_sum_to_all((int *)sout, (int *)sin, elems, 0, 0, _c->size, (int *)pWrk, pSync);
          break;
        case LongType:
        case UnsignedLongType:
          shmem_long_sum_to_all((long *)sout, (long *)sin, elems, 0, 0, _c->size, (long *)pWrk, pSync);
          break;
        case DoubleType:
          shmem_double_sum_to_all((double *)sout, (double *)sin, elems, 0, 0, _c->size, (double *)pWrk, pSync);
          break;
        case CharType:
        case BoolType:
        default:
          gtc_dprintf("gtc_reduce: unsupported reduction datatype\n");
          exit(1);
      }
    case GtcReduceOpMin:
      switch(type) {
        case IntType:
          shmem_int_min_to_all((int *)sout, (int *)sin, elems, 0, 0, _c->size, (int *)pWrk, pSync);
          break;
        case LongType:
        case UnsignedLongType:
          shmem_long_min_to_all((long *)sout, (long *)sin, elems, 0, 0, _c->size, (long *)pWrk, pSync);
          break;
        case DoubleType:
          shmem_double_min_to_all((double *)sout, (double *)sin, elems, 0, 0, _c->size, (double *)pWrk, pSync);
          break;
        case CharType:
        case BoolType:
        default:
          gtc_dprintf("gtc_reduce: unsupported reduction datatype\n");
          exit(1);
      }
    case GtcReduceOpMax:
      switch(type) {
        case IntType:
          shmem_int_max_to_all((int *)sout, (int *)sin, elems, 0, 0, _c->size, (int *)pWrk, pSync);
          break;
        case UnsignedLongType:
        case LongType:
          shmem_long_max_to_all((long *)sout, (long *)sin, elems, 0, 0, _c->size, (long *)pWrk, pSync);
          break;
        case DoubleType:
          shmem_double_max_to_all((double *)sout, (double *)sin, elems, 0, 0, _c->size, (double *)pWrk, pSync);
          break;
        case CharType:
        case BoolType:
        default:
          gtc_dprintf("gtc_reduce: unsupported reduction datatype\n");
          exit(1);
      }
      break;
    default:
      gtc_dprintf("gtc_reduce: unsupported reduction operation\n");
      exit(1);
  }

  memcpy(out, sout, elems * gtc_collective_size(type));

  shmem_free(sin);
  shmem_free(sout);
  shmem_free(pWrk);
  return GtcStatusOK;
}



/**
 * gtc_collective_size - helper function to return data type size for collective ops
 * @param type PDHT datatype
 * @returns size of datatype
 */
static int gtc_collective_size(gtc_datatype_t type) {
  int tysize;
  switch(type) {
    case IntType:
      tysize = sizeof(int);
      break;
    case LongType:
    case UnsignedLongType:
      tysize = sizeof(long);
      break;
    case DoubleType:
      tysize = sizeof(double);
      break;
    case CharType:
    case BoolType:
      tysize = sizeof(char);
      break;
    default:
      gtc_dprintf("gtc_reduce: unsupported reduction datatype\n");
      exit(1);
  }
  return tysize;
}
