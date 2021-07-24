/*
 * Copyright (c) 2021. See COPYRIGHT in top-level directory.
 */

#pragma once

#define SYNCH_RMW_OP ARMCI_SWAP
struct synch_mutex_s {
  long *locks;
};

typedef struct synch_mutex_s synch_mutex_t;

// Statistics measuring lock contention
extern double        synch_mutex_lock_nattempts_squares;
extern unsigned long synch_mutex_lock_nattempts_last;
extern unsigned long synch_mutex_lock_nattempts_sum;
extern unsigned long synch_mutex_lock_nattempts_max;
extern unsigned long synch_mutex_lock_nattempts_min;
extern unsigned long synch_mutex_lock_ncalls_contention;
extern unsigned long synch_mutex_lock_ncalls;

void synch_mutex_init(synch_mutex_t *lock);
void synch_mutex_lock(synch_mutex_t *lock, int proc);
int  synch_mutex_trylock(synch_mutex_t *lock, int proc);
void synch_mutex_unlock(synch_mutex_t *lock, int proc);
