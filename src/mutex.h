#ifndef _mutex_h
#define _mutex_h

//#define SYNCH_RMW_OP ARMCI_SWAP_LONG
//typedef long synch_mutex_t;

#define SYNCH_RMW_OP ARMCI_SWAP
typedef int synch_mutex_t;

extern long *lock;

// Statistics measuring lock contention
extern double        synch_mutex_lock_nattempts_squares;
extern unsigned long synch_mutex_lock_nattempts_last;
extern unsigned long synch_mutex_lock_nattempts_sum;
extern unsigned long synch_mutex_lock_nattempts_max;
extern unsigned long synch_mutex_lock_nattempts_min;
extern unsigned long synch_mutex_lock_ncalls_contention;
extern unsigned long synch_mutex_lock_ncalls;

void synch_mutex_init(long *lock, int proc);
void synch_mutex_lock(long *lock, int proc);
int  synch_mutex_trylock(long *lock, int proc);
void synch_mutex_unlock(long *lock, int proc);

#endif /* _mutex_h */
